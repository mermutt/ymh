#include "ymh/agent/subagent_service.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "ymh/agent/agent_loop.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/agent/session_activator.hpp"
#include "ymh/jobs/job_wakeup.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {
namespace {

constexpr std::string_view kSubagentKind     = "subagent";
constexpr auto             kSettlementTimeout = std::chrono::seconds(120);

Message user_prompt(const std::string& text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = text;
    message.content = {std::move(block)};
    return message;
}

std::string last_assistant_text(const Session& session) {
    const EventRange events = session.events();
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->event.type != EventType::AssistantMessage) {
            continue;
        }
        const auto& message = it->event.payload.get<payload::AssistantMessage>();
        std::string text;
        for (const ContentBlock& block : message.content) {
            if (block.kind == ContentBlockKind::Text) {
                text += block.text;
            }
        }
        return text;
    }
    return {};
}

struct ChildTerminal {
    payload::SubagentOutcome    outcome = payload::SubagentOutcome::Completed;
    std::string                 summary;
    std::optional<FinishReason> finish;
};

ChildTerminal read_terminal(const Session& session) {
    const EventRange events = session.events();
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        switch (it->event.type) {
            case EventType::TurnEnded: {
                ChildTerminal terminal;
                terminal.outcome = payload::SubagentOutcome::Completed;
                terminal.finish  = it->event.payload.get<payload::TurnEnded>().finish_reason;
                return terminal;
            }
            case EventType::TurnFailed: {
                ChildTerminal terminal;
                terminal.outcome = payload::SubagentOutcome::Failed;
                terminal.summary = it->event.payload.get<payload::TurnFailed>().message;
                return terminal;
            }
            case EventType::TurnCancelled: {
                ChildTerminal terminal;
                terminal.outcome = payload::SubagentOutcome::Cancelled;
                return terminal;
            }
            default:
                break;
        }
    }
    return {};
}

std::string stop_headline(payload::SubagentOutcome outcome,
                          const std::optional<FinishReason>& finish) {
    if (outcome == payload::SubagentOutcome::Cancelled) {
        return "subagent run was cancelled";
    }
    if (outcome == payload::SubagentOutcome::Failed) {
        return "subagent run failed";
    }
    if (finish.has_value()) {
        if (*finish == FinishReason::Length) {
            return "subagent run hit its token limit before finishing";
        }
        if (*finish == FinishReason::ContentFilter) {
            return "subagent declined the task";
        }
        if (*finish != FinishReason::Stop && *finish != FinishReason::ToolCalls) {
            return "subagent run ended abnormally (" + std::string{to_string(*finish)} + ")";
        }
    }
    return {};
}

JobStatus job_status_for(payload::SubagentOutcome outcome) {
    switch (outcome) {
        case payload::SubagentOutcome::Completed:
            return JobStatus::Completed;
        case payload::SubagentOutcome::Cancelled:
            return JobStatus::Killed;
        case payload::SubagentOutcome::Failed:
            return JobStatus::Failed;
    }
    return JobStatus::Failed;
}

std::string settlement_correlation(const SessionId& child, std::size_t ordinal) {
    return "subagent-settlement:" + child.value + "#" + std::to_string(ordinal);
}

std::string render_notice(const SessionId& child, const std::string& closing) {
    return "Background subagent " + child.value +
           " finished and will do no further work unless you send it more.\nIts closing message:\n" +
           (closing.empty() ? std::string{"(no output)"} : closing);
}

std::size_t count_fanin(const Session& parent, const SessionId& child) {
    std::size_t count = 0;
    for (const EventRecord& record : parent.events()) {
        if (record.event.type != EventType::SubagentFanIn) {
            continue;
        }
        if (record.event.payload.get<payload::SubagentFanIn>().subagent.value == child.value) {
            ++count;
        }
    }
    return count;
}

std::optional<MessageSource> notice_source(const Event& event) {
    if (event.type == EventType::UserMessage) {
        return event.payload.get<payload::UserMessage>().source;
    }
    if (event.type == EventType::ContextInjected) {
        return event.payload.get<payload::ContextInjected>().source;
    }
    return std::nullopt;
}

bool wait_for_settlement(const std::shared_ptr<AgentLoop>& child, const Message& message) {
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
    child->send(message);
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_for(lock, kSettlementTimeout, [&] { return state->done; });
}

} // namespace

struct SubagentService::Impl {
    AgentRegistry&            registry;
    SessionManager&           sessions;
    AgentPresetRoster*        presets;
    JobRegistry&              jobs;
    JobWakeupPolicy&          wakeup;
    ModelSelectionController& model_selection;
    RouteCatalog&             route_catalog;
    LlmRuntime&               llm;
    SessionActivator&         activator;
    EventBus&                 bus;

    std::mutex mutex;
    struct Epoch {
        SessionId            parent;
        bool                 notice_expected = false;
        bool                 continuable = false;
        bool                 open = false;
        std::optional<JobId> job;
    };
    std::unordered_map<std::string, Epoch>   epochs;
    std::unordered_map<std::string, Message> pending_prompts;

    Impl(AgentRegistry& registry_in, SessionManager& sessions_in, AgentPresetRoster* presets_in,
         JobRegistry& jobs_in, JobWakeupPolicy& wakeup_in,
         ModelSelectionController& model_selection_in, RouteCatalog& route_catalog_in,
         LlmRuntime& llm_in, SessionActivator& activator_in, EventBus& bus_in)
        : registry(registry_in),
          sessions(sessions_in),
          presets(presets_in),
          jobs(jobs_in),
          wakeup(wakeup_in),
          model_selection(model_selection_in),
          route_catalog(route_catalog_in),
          llm(llm_in),
          activator(activator_in),
          bus(bus_in) {}

    [[nodiscard]] std::uint32_t max_depth() const noexcept {
        return presets != nullptr ? presets->config().max_depth : 3;
    }

    std::string child_output(const SessionId& child) {
        try {
            const std::shared_ptr<Session> session = sessions.sessionPtr(child);
            return last_assistant_text(*session);
        } catch (const std::exception&) {
            return {};
        }
    }

    ChildTerminal child_terminal(const SessionId& child) {
        try {
            const std::shared_ptr<Session> session = sessions.sessionPtr(child);
            return read_terminal(*session);
        } catch (const std::exception&) {
            return {};
        }
    }

    std::size_t fanin_count(const SessionId& parent, const SessionId& child) {
        try {
            const std::shared_ptr<Session> session = sessions.sessionPtr(parent);
            return count_fanin(*session, child);
        } catch (const std::exception&) {
            return 0;
        }
    }

    std::expected<ChildRoute, AgentError> resolve_route(
        const SessionId& parent_session, const std::optional<ChildAgentOptions>& options) {
        const bool has_endpoint =
            options.has_value() && options->endpoint.has_value() && !options->endpoint->empty();
        const bool has_model =
            options.has_value() && options->model.has_value() && !options->model->empty();
        if (has_endpoint != has_model) {
            return std::unexpected(
                AgentError{AgentErrorCode::Internal, "subagent route requires provider and model"});
        }

        ChildRoute route;
        if (has_endpoint) {
            route.endpoint   = *options->endpoint;
            route.profile_id = options->profile_id;
            route.model      = *options->model;
            route.model_name = "";
            route.max_tokens = options->max_tokens;
            if (options->reasoning_effort.has_value()) {
                route.reasoning_effort = options->reasoning_effort;
            }
            if (!route_catalog.is_routable_endpoint(route.endpoint)) {
                return std::unexpected(AgentError{AgentErrorCode::ProviderFailed,
                                                  "unknown endpoint: " + route.endpoint});
            }
            if (!route_catalog.is_catalog_member(route.endpoint, route.model)) {
                return std::unexpected(AgentError{AgentErrorCode::ProviderFailed,
                                                  "model is not a catalog member: " + route.model});
            }
            if (route.reasoning_effort.has_value()) {
                const std::vector<std::string> efforts =
                    route_catalog.efforts_for(route.endpoint, route.model);
                if (!efforts.empty() &&
                    std::find(efforts.begin(), efforts.end(), *route.reasoning_effort) ==
                        efforts.end()) {
                    return std::unexpected(
                        AgentError{AgentErrorCode::ProviderFailed, "unsupported reasoning_effort"});
                }
            }
        } else {
            std::optional<ModelSelection> base;
            if (const std::shared_ptr<AgentLoop> agent = registry.findShared(parent_session)) {
                base = agent->effective_model_selection();
            } else {
                try {
                    const std::shared_ptr<Session> session = sessions.sessionPtr(parent_session);
                    base = model_selection.effective(*session);
                } catch (const std::exception&) {
                }
            }
            if (!base.has_value()) {
                return std::unexpected(
                    AgentError{AgentErrorCode::ProviderFailed, "no parent route to inherit"});
            }
            route.endpoint         = base->endpoint;
            route.profile_id       = base->profile.id;
            route.model            = base->model;
            route.model_name       = base->model_name;
            route.max_tokens       = options.has_value() ? options->max_tokens : std::nullopt;
            route.reasoning_effort = base->parameters.reasoning_effort;
            if (options.has_value() && options->reasoning_effort.has_value()) {
                route.reasoning_effort = options->reasoning_effort;
            }
        }

        LlmCallConfig config;
        config.endpoint         = route.endpoint;
        config.profile_id       = route.profile_id.value_or("");
        config.model            = route.model;
        config.reasoning_effort = route.reasoning_effort;
        config.max_tokens       = route.max_tokens;
        try {
            llm.preflight_route(config);
        } catch (const NoProviderRouteError& error) {
            return std::unexpected(AgentError{AgentErrorCode::ProviderFailed, error.detail});
        }
        return route;
    }

    std::expected<SessionId, AgentError> create_child(const SessionId& parent_session,
                                                      const StartRequest& request,
                                                      const ChildRoute&   route) {
        std::shared_ptr<Session> parent;
        try {
            parent = sessions.sessionPtr(parent_session);
        } catch (const std::exception& error) {
            return std::unexpected(AgentError{AgentErrorCode::UnknownSession, error.what()});
        }
        const SessionHeader header = parent->header();

        SessionOptions options;
        options.cwd               = header.cwd;
        options.serverProfile     = header.serverProfile;
        options.model             = route.model;
        options.model_name        = route.model_name;
        options.title             = request.label;
        options.kind              = SessionKind::Subagent;
        options.parentSession     = parent_session;
        options.depth             = header.depth + 1;
        options.agent_preset      = header.agent_preset;
        options.permission_preset = header.permission_preset;
        options.endpoint          = route.endpoint;
        options.profile_id        = route.profile_id;
        options.reasoning_effort  = route.reasoning_effort;
        options.max_tokens        = route.max_tokens;

        ChildSpawnRequest spawn;
        spawn.options      = options;
        spawn.parent_depth = header.depth;
        spawn.max_depth    = max_depth();
        spawn.composition  = request.composition;
        spawn.route        = route;

        const std::expected<AgentId, AgentError> created = registry.createChild(spawn);
        if (!created.has_value()) {
            return std::unexpected(created.error());
        }
        const std::shared_ptr<AgentLoop> child = registry.getShared(*created);
        if (child == nullptr) {
            return std::unexpected(
                AgentError{AgentErrorCode::Internal, "subagent disposed during creation"});
        }
        return child->session();
    }

    void append_spawned(const SessionId& parent_session, const SessionId& child,
                        const std::string& task) {
        try {
            payload::SubagentSpawned spawned;
            spawned.subagent = child;
            spawned.task     = task;
            sessions.sessionPtr(parent_session)->append(spawned);
        } catch (const std::exception&) {
        }
    }

    void open_epoch(const SessionId& child, const SessionId& parent, bool notice_expected,
                    bool continuable, const std::string& label) {
        Epoch epoch;
        epoch.parent          = parent;
        epoch.notice_expected = notice_expected;
        epoch.continuable     = continuable;
        epoch.open            = true;

        if (notice_expected) {
            AgentId owner{};
            if (const std::shared_ptr<AgentLoop> parent_agent = registry.findShared(parent)) {
                owner = parent_agent->id();
            }
            std::size_t existing = 0;
            try {
                existing = fanin_count(parent, child);
            } catch (const std::exception&) {
            }
            JobStart start;
            start.kind          = std::string{kSubagentKind};
            start.label         = label.empty() ? child.value : label;
            start.owner         = owner;
            start.notice_plugin = settlement_correlation(child, existing + 1);
            start.notice_text   = "Background subagent " + child.value + " started.";
            start.run           = [this, child]() {
                JobHooks hooks;
                hooks.read_output = [this, child]() {
                    try {
                        return child_output(child);
                    } catch (const std::exception&) {
                        return std::string{};
                    }
                };
                hooks.cancel = [this, child](std::string_view) {
                    if (const std::shared_ptr<AgentLoop> agent = registry.findShared(child)) {
                        agent->cancel();
                    }
                };
                return hooks;
            };
            epoch.job = jobs.start(std::move(start));
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            epochs[child.value] = epoch;
        }
        if (notice_expected) {
            watch_settlement(child, parent, notice_expected, continuable);
        }
    }

    void watch_settlement(const SessionId& child, const SessionId& parent, bool notice_expected,
                          bool continuable) {
        const std::shared_ptr<AgentLoop> agent = registry.findShared(child);
        if (agent == nullptr) {
            return;
        }
        agent->onSettled([this, child, parent, notice_expected, continuable]() {
            const std::shared_ptr<AgentLoop> live = registry.findShared(child);
            if (live != nullptr && live->hasPendingWork()) {
                watch_settlement(child, parent, notice_expected, continuable);
                return;
            }
            std::string              closing;
            payload::SubagentOutcome outcome = payload::SubagentOutcome::Completed;
            try {
                const ChildTerminal terminal = child_terminal(child);
                outcome = terminal.outcome;
                closing = terminal.summary;
                if (closing.empty()) {
                    closing = child_output(child);
                }
            } catch (const std::exception&) {
            }
            (void)settle_epoch(parent, child, outcome, closing, notice_expected);
            if (!continuable && live != nullptr) {
                registry.dispose(live->id());
            }
        });
    }

    std::expected<SettlementResult, AgentError> settle_epoch(const SessionId& parent,
                                                             const SessionId& child,
                                                             payload::SubagentOutcome outcome,
                                                             std::string summary,
                                                             bool notice_expected) {
        SettlementResult result;
        result.outcome = outcome;

        std::shared_ptr<Session> parent_session;
        try {
            parent_session = sessions.sessionPtr(parent);
        } catch (const std::exception& error) {
            return std::unexpected(AgentError{AgentErrorCode::UnknownSession, error.what()});
        }

        payload::SubagentFanIn fan_in;
        fan_in.subagent        = child;
        fan_in.outcome         = outcome;
        fan_in.summary         = summary;
        fan_in.notice_expected = notice_expected;
        parent_session->append(fan_in);

        std::optional<JobId> job;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto                  it = epochs.find(child.value);
            if (it != epochs.end()) {
                job        = it->second.job;
                it->second.open = false;
            }
        }
        if (job.has_value()) {
            JobOutcome job_outcome;
            job_outcome.status = job_status_for(outcome);
            if (!summary.empty()) {
                job_outcome.detail = summary;
            }
            job_outcome.notice_text = render_notice(child, summary);
            jobs.settle(*job, std::move(job_outcome));
        }
        return result;
    }

    bool authorize(const SessionId& sender, const SessionId& target) {
        if (sender.value == target.value) {
            return false;
        }
        auto header_of = [this](const SessionId& id) -> std::optional<SessionHeader> {
            try {
                return sessions.sessionPtr(id)->header();
            } catch (const std::exception&) {
            }
            try {
                sessions.resumeSession(id);
                return sessions.sessionPtr(id)->header();
            } catch (const std::exception&) {
                return std::nullopt;
            }
        };
        const std::optional<SessionHeader> target_header = header_of(target);
        if (!target_header.has_value()) {
            return false;
        }
        if (target_header->parentSession.has_value() &&
            target_header->parentSession->value == sender.value) {
            return true;
        }
        const std::optional<SessionHeader> sender_header = header_of(sender);
        return sender_header.has_value() && sender_header->parentSession.has_value() &&
               sender_header->parentSession->value == target.value;
    }

    std::vector<ChildDescriptor> children_of(const SessionId& caller, bool descendants) const {
        std::vector<ChildDescriptor>    out;
        std::vector<SessionId>          frontier{caller};
        std::unordered_set<std::string> seen;
        while (!frontier.empty()) {
            std::vector<SessionId> next;
            for (const SessionId& current : frontier) {
                for (const AgentId& agent_id : registry.list()) {
                    const std::shared_ptr<AgentLoop> agent = registry.getShared(agent_id);
                    if (agent == nullptr) {
                        continue;
                    }
                    ChildDescriptor descriptor;
                    try {
                        const SessionHeader header = sessions.sessionPtr(agent->session())->header();
                        if (!header.parentSession.has_value() ||
                            header.parentSession->value != current.value) {
                            continue;
                        }
                        if (header.kind != SessionKind::Subagent) {
                            continue;
                        }
                        if (!seen.insert(header.id.value).second) {
                            continue;
                        }
                        descriptor.child  = header.id;
                        descriptor.parent = header.parentSession;
                        descriptor.depth  = header.depth;
                        descriptor.label  = header.title;
                        descriptor.status = (agent->status() == AgentStatus::Running ||
                                             agent->hasPendingWork())
                                                ? ChildDescriptor::Status::Running
                                                : ChildDescriptor::Status::Idle;
                    } catch (const std::exception& error) {
                        descriptor.diagnostic = error.what();
                    }
                    out.push_back(std::move(descriptor));
                    if (descendants) {
                        next.push_back(out.back().child);
                    }
                }
            }
            frontier = std::move(next);
        }
        return out;
    }
};

SubagentService::SubagentService(AgentRegistry& registry, SessionManager& sessions,
                                 AgentPresetRoster* presets, JobRegistry& jobs,
                                 JobWakeupPolicy& wakeup,
                                 ModelSelectionController& model_selection,
                                 RouteCatalog& route_catalog, LlmRuntime& llm,
                                 SessionActivator& activator, EventBus& bus)
    : impl_(std::make_unique<Impl>(registry, sessions, presets, jobs, wakeup, model_selection,
                                   route_catalog, llm, activator, bus)) {}

SubagentService::~SubagentService() = default;

std::uint32_t SubagentService::max_depth() const noexcept { return impl_->max_depth(); }

Task<StartResult> SubagentService::startOneShot(StartRequest request) {
    StartResult result;
    result.kind = StartResult::Kind::Foreground;

    const std::expected<ChildRoute, AgentError> route =
        impl_->resolve_route(request.parent, request.agent_options);
    if (!route.has_value()) {
        result.error = route.error();
        return Task<StartResult>(std::move(result));
    }
    const std::expected<SessionId, AgentError> child =
        impl_->create_child(request.parent, request, *route);
    if (!child.has_value()) {
        result.error = child.error();
        return Task<StartResult>(std::move(result));
    }
    result.child = *child;
    impl_->append_spawned(request.parent, *child, request.prompt);

    if (request.run_in_background) {
        impl_->open_epoch(*child, request.parent, true, false, request.label);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->pending_prompts[child->value] = user_prompt(request.prompt);
        }
        if (!impl_->activator.submit(*child)) {
            const std::shared_ptr<AgentLoop> created = impl_->registry.findShared(*child);
            if (created != nullptr) {
                impl_->registry.dispose(created->id());
            }
            result.error = AgentError{AgentErrorCode::InboxFull, "executor queue full"};
            return Task<StartResult>(std::move(result));
        }
        std::optional<JobId> job;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            job = impl_->epochs[child->value].job;
        }
        result.kind   = StartResult::Kind::BackgroundJob;
        result.job_id = job.has_value() ? to_string(*job) : std::string{};
        return Task<StartResult>(std::move(result));
    }

    const std::shared_ptr<AgentLoop> agent = impl_->registry.findShared(*child);
    if (agent == nullptr || !wait_for_settlement(agent, user_prompt(request.prompt))) {
        result.error = AgentError{AgentErrorCode::Cancelled, "subagent did not settle"};
        return Task<StartResult>(std::move(result));
    }
    const ChildTerminal terminal = impl_->child_terminal(*child);
    const std::string   headline = stop_headline(terminal.outcome, terminal.finish);
    if (!headline.empty()) {
        result.error = AgentError{terminal.outcome == payload::SubagentOutcome::Cancelled
                                      ? AgentErrorCode::Cancelled
                                      : AgentErrorCode::ProviderFailed,
                                  headline};
    }
    const std::string closing = terminal.summary.empty()
                                    ? impl_->child_output(*child)
                                    : terminal.summary;
    result.text = headline.empty() ? impl_->child_output(*child) : headline;
    (void)impl_->settle_epoch(request.parent, *child, terminal.outcome, closing, false);
    impl_->registry.dispose(agent->id());
    return Task<StartResult>(std::move(result));
}

Task<StartResult> SubagentService::startContinuable(StartRequest request) {
    StartResult result;
    result.kind = StartResult::Kind::Continuable;

    const std::expected<ChildRoute, AgentError> route =
        impl_->resolve_route(request.parent, request.agent_options);
    if (!route.has_value()) {
        result.error = route.error();
        return Task<StartResult>(std::move(result));
    }
    const std::expected<SessionId, AgentError> child =
        impl_->create_child(request.parent, request, *route);
    if (!child.has_value()) {
        result.error = child.error();
        return Task<StartResult>(std::move(result));
    }
    result.child = *child;
    impl_->append_spawned(request.parent, *child, request.prompt);

    if (request.run_in_background) {
        impl_->open_epoch(*child, request.parent, true, true, request.label);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->pending_prompts[child->value] = user_prompt(request.prompt);
        }
        if (!impl_->activator.submit(*child)) {
            const std::shared_ptr<AgentLoop> created = impl_->registry.findShared(*child);
            if (created != nullptr) {
                impl_->registry.dispose(created->id());
            }
            result.error = AgentError{AgentErrorCode::InboxFull, "executor queue full"};
            return Task<StartResult>(std::move(result));
        }
        return Task<StartResult>(std::move(result));
    }

    impl_->open_epoch(*child, request.parent, false, true, request.label);
    const std::shared_ptr<AgentLoop> agent = impl_->registry.findShared(*child);
    if (agent == nullptr || !wait_for_settlement(agent, user_prompt(request.prompt))) {
        result.error = AgentError{AgentErrorCode::Cancelled, "subagent did not settle"};
        return Task<StartResult>(std::move(result));
    }
    const ChildTerminal terminal = impl_->child_terminal(*child);
    const std::string   headline = stop_headline(terminal.outcome, terminal.finish);
    if (!headline.empty()) {
        result.error = AgentError{terminal.outcome == payload::SubagentOutcome::Cancelled
                                      ? AgentErrorCode::Cancelled
                                      : AgentErrorCode::ProviderFailed,
                                  headline};
    }
    const std::string closing = terminal.summary.empty()
                                    ? impl_->child_output(*child)
                                    : terminal.summary;
    result.text = headline.empty() ? impl_->child_output(*child) : headline;
    (void)impl_->settle_epoch(request.parent, *child, terminal.outcome, closing, false);
    return Task<StartResult>(std::move(result));
}

bool SubagentService::activateChild(const SessionId& child) {
    Message message;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto                  it = impl_->pending_prompts.find(child.value);
        if (it == impl_->pending_prompts.end()) {
            return false;
        }
        message = it->second;
        impl_->pending_prompts.erase(it);
    }
    const std::shared_ptr<AgentLoop> agent = impl_->registry.findShared(child);
    if (agent == nullptr) {
        return false;
    }
    agent->send(std::move(message));
    return true;
}

Task<SendResult> SubagentService::sendMessage(const SessionId& sender, const SessionId& target,
                                              const std::string& message) {
    SendResult result;
    if (!impl_->authorize(sender, target)) {
        result.error = AgentError{AgentErrorCode::Internal, "target is not a direct child"};
        return Task<SendResult>(std::move(result));
    }
    std::shared_ptr<AgentLoop> agent = impl_->registry.findShared(target);
    if (agent == nullptr) {
        try {
            impl_->sessions.resumeSession(target);
            const std::expected<AgentId, AgentError> resumed = impl_->registry.resume(target);
            if (!resumed.has_value()) {
                result.error = resumed.error();
                return Task<SendResult>(std::move(result));
            }
        } catch (const std::exception& error) {
            result.error = AgentError{AgentErrorCode::UnknownSession, error.what()};
            return Task<SendResult>(std::move(result));
        }
        agent = impl_->registry.findShared(target);
    }
    if (agent == nullptr || agent->disposed()) {
        result.error = AgentError{AgentErrorCode::AgentDisposed, "target unavailable"};
        return Task<SendResult>(std::move(result));
    }

    bool epoch_open = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto                  it = impl_->epochs.find(target.value);
        epoch_open = it != impl_->epochs.end() && it->second.open;
    }
    if (!epoch_open) {
        impl_->open_epoch(target, sender, true, true, "");
    }

    Message relay = user_prompt("Agent " + sender.value + " sent a message:\n" + message);
    relay.source         = message_source(MessageSource::Kind::Plugin);
    relay.source->plugin = "agent-message";
    relay.source->sender = sender.value;
    relay.context        = ContextFormed{ContextForm::Relay};

    const InboxResult accepted = (agent->status() != AgentStatus::Idle || agent->hasPendingWork())
                                     ? agent->steer(relay)
                                     : agent->followup(relay);
    if (accepted != InboxResult::Accepted) {
        result.error =
            AgentError{accepted == InboxResult::InboxFull ? AgentErrorCode::InboxFull
                                                          : AgentErrorCode::AgentDisposed,
                       "message was not delivered"};
        return Task<SendResult>(std::move(result));
    }
    result.message_id = make_event_id().value;
    return Task<SendResult>(std::move(result));
}

Task<void> SubagentService::interrupt(const SessionId& sender, const SessionId& target) {
    if (!impl_->authorize(sender, target)) {
        return Task<void>{};
    }
    if (const std::shared_ptr<AgentLoop> agent = impl_->registry.findShared(target)) {
        agent->cancel();
    }
    return Task<void>{};
}

std::vector<ChildDescriptor> SubagentService::list(const SessionId& caller, ListScope scope) const {
    return impl_->children_of(caller, scope == ListScope::Descendants);
}

std::expected<SettlementResult, AgentError> SubagentService::settle(
    const SessionId& parent, const SessionId& child, payload::SubagentOutcome outcome,
    std::string summary, bool notice_expected) {
    return impl_->settle_epoch(parent, child, outcome, std::move(summary), notice_expected);
}

void SubagentService::replayUnreportedSettlements(const SessionId& parent) {
    std::shared_ptr<Session> session;
    try {
        session = impl_->sessions.sessionPtr(parent);
    } catch (const std::exception&) {
        return;
    }

    std::unordered_map<std::string, std::size_t>   ordinals;
    std::vector<std::pair<SessionId, std::size_t>> pending;
    std::unordered_set<std::string>                delivered;
    for (const EventRecord& record : session->events()) {
        if (record.event.type == EventType::SubagentFanIn) {
            const auto&       fan_in  = record.event.payload.get<payload::SubagentFanIn>();
            const std::size_t ordinal = ++ordinals[fan_in.subagent.value];
            if (fan_in.notice_expected) {
                pending.emplace_back(fan_in.subagent, ordinal);
            }
            continue;
        }
        const std::optional<MessageSource> source = notice_source(record.event);
        if (source.has_value() && !source->plugin.empty()) {
            delivered.insert(source->plugin);
        }
    }

    for (const auto& [child, ordinal] : pending) {
        const std::string correlation = settlement_correlation(child, ordinal);
        if (delivered.contains(correlation)) {
            continue;
        }
        payload::ContextInjected notice;
        notice.id            = make_event_id().value;
        notice.role          = Role::User;
        notice.text          = render_notice(child, "");
        notice.source        = message_source(MessageSource::Kind::Plugin);
        notice.source.plugin = correlation;
        notice.context       = ContextFormed{ContextForm::Notice, {}, notice.text};
        session->append(notice);
        delivered.insert(correlation);
    }
}

} // namespace ymh

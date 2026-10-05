#include "ymh/agent/agent_loop.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "ymh/agent/preset.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/llm/assistant_stream.hpp"
#include "ymh/prompt/instructions.hpp"
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/tool.hpp"
#include "ymh/tools/tool_context.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh {
namespace {

std::string assembled_system_prompt(const LLMRequest& request) {
    if (request.messages.empty() || request.messages.front().role != Role::System) {
        return {};
    }
    std::string text;
    for (const ContentBlock& block : request.messages.front().content) {
        if (block.kind == ContentBlockKind::Text) {
            text += block.text;
        }
    }
    return text;
}

std::string first_line(const std::string& text) {
    const std::size_t newline = text.find('\n');
    return text.substr(0, newline == std::string::npos ? text.size() : newline);
}

// 73-D1: the settled assistant text is the concatenation of the Text blocks
// only; reasoning/thinking blocks are excluded, so a reasoning-only completion
// with no visible answer and no tool call is a non-action.
std::string text_of_blocks(const std::vector<ContentBlock>& blocks) {
    std::string text;
    for (const ContentBlock& block : blocks) {
        if (block.kind == ContentBlockKind::Text) {
            text += block.text;
        }
    }
    return text;
}

// 73-D1: "blank" is empty or whitespace-only. It is a purely structural test;
// no identity, vendor, model-name, or prompt-wording inspection happens here.
bool is_blank(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char character) { return std::isspace(character) != 0; });
}

// 73-D9: structural normalization for cross-turn repetition. ASCII-only and
// locale-independent: trim leading/trailing ASCII whitespace, collapse every run
// of ASCII whitespace to one space, and ASCII-lowercase A-Z. No Unicode
// normalization and no punctuation stripping — those would merge distinct texts
// and raise false positives.
std::string normalize_reply(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const char raw : text) {
        const unsigned char character = static_cast<unsigned char>(raw);
        const bool          ascii_space =
            character == ' ' || character == '\t' || character == '\n' ||
            character == '\r' || character == '\f' || character == '\v';
        if (ascii_space) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        if (character >= 'A' && character <= 'Z') {
            out.push_back(static_cast<char>(character + ('a' - 'A')));
        } else {
            out.push_back(raw);
        }
    }
    return out;
}

// 73-D9: how many distinct normalized prompts one repeated answer must be seen
// against before the repetition arm fires; the current turn counts as one, so
// the predicate needs `kRepeatOccurrences - 1` prior distinct-prompt matches.
constexpr std::size_t kRepeatOccurrences = 3;
// 73-D10: fixed bound on the per-session observation ring.
constexpr std::size_t kAnswerLogCapacity = 8;
// 73-D11: how many completed turns must carry the exact same normalized
// `(prompt, answer)` pair before the same-prompt repetition arm fires. The
// current turn's just-recorded observation counts, so the predicate needs
// `kSamePromptRepeatOccurrences - 1` prior identical pairs.
constexpr std::size_t kSamePromptRepeatOccurrences = 3;

// 73-D2: the single bounded corrective nudge. It is a user-role message so the
// provider presents it as the latest instruction; its Plugin source keeps it out
// of the composer history and the user-message job-wakeup path while still
// rendering visibly (73-D6).
constexpr std::string_view kNudgeText =
    "No answer or tool call was produced. Continue the task now: call the "
    "appropriate tool, or give the final answer in text.";

Message make_nudge_message() {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::string{kNudgeText};
    message.content.push_back(std::move(block));
    MessageSource source;
    source.kind   = MessageSource::Kind::Plugin;
    source.plugin = "agent-nudge";
    message.source = std::move(source);
    return message;
}

// 62-D6 (Rev 3): progress is novelty, not success. A step's *action signature*
// digests the ordered tool calls (name + canonical arguments); its *result
// signature* digests the ordered committed results (name + outcome + error +
// output). Both exclude the per-call unique id. A step makes progress iff its
// action signature is novel in the turn, OR it commits at least one successful
// (`Ok`) result whose result signature is novel. A distinct informative failure
// (a new path, a new command, a new error) is therefore progress and does not
// trip the guard, while a repeated identical call — or a periodic A/B cycle —
// eventually repeats and is counted as no progress. `kNoProgressStreak`
// consecutive non-progress steps stop the turn.
constexpr std::size_t kNoProgressStreak = 3;

std::string action_signature(const std::vector<ToolCallAssembled>& calls) {
    std::string material;
    for (const ToolCallAssembled& call : calls) {
        material += call.name;
        material += '\x1f';
        material += call.arguments.dump();
        material += '\x1e';
    }
    return sha256_hex(material);
}

std::string result_signature(const ToolScheduleOutcome& outcome) {
    std::string material;
    for (const payload::ToolResult& result : outcome.results) {
        material += result.name;
        material += '\x1f';
        material += std::string{tool_outcome_name(result.outcome)};
        material += '\x1f';
        material += result.error.value_or(std::string{});
        material += '\x1f';
        material += result.output;
        material += '\x1e';
    }
    return sha256_hex(material);
}

bool step_had_success(const ToolScheduleOutcome& outcome) {
    return std::any_of(outcome.results.begin(), outcome.results.end(),
                       [](const payload::ToolResult& result) {
                           return result.outcome == payload::ToolOutcome::Ok;
                       });
}

bool has_valid_plan_argument(const nlohmann::json& arguments) {
    if (!arguments.is_object()) {
        return false;
    }
    const auto plan = arguments.find("plan");
    if (plan == arguments.end() || !plan->is_string()) {
        return false;
    }
    return plan->get_ref<const std::string&>().find_first_not_of(" \t\r\n") != std::string::npos;
}

// 25-D5/N5 + 53-D7: commits any queued plan/model selection on every turn exit
// (normal end, error, cancellation, lease/store failure, step limit), including
// maintenance.
class TurnFlushGuard {
public:
    TurnFlushGuard(PlanModeController* plan, ModelSelectionController* model, Session& session)
        : plan_(plan), model_(model), session_(session) {}
    TurnFlushGuard(const TurnFlushGuard&) = delete;
    TurnFlushGuard& operator=(const TurnFlushGuard&) = delete;
    ~TurnFlushGuard() {
        if (plan_ != nullptr) {
            plan_->flush_pending_at_turn_end(session_);
        }
        if (model_ != nullptr) {
            model_->flush_pending_at_turn_end(session_);
        }
    }

private:
    PlanModeController*      plan_;
    ModelSelectionController* model_;
    Session&                 session_;
};

} // namespace

AgentLoop::AgentLoop(AgentId id, std::shared_ptr<Session> session, AgentServices services,
                     AgentConfig config)
    : id_(std::move(id)),
      session_owner_(std::move(session)),
      session_(*session_owner_),
      services_(std::move(services)),
      config_(std::move(config)) {
    const EventRange events = session_.events();
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->event.type != EventType::LlmRequestHeader) {
            continue;
        }
        const auto& header     = it->event.payload.get<payload::LlmRequestHeader>();
        held_config_           = header.config;
        held_purpose_          = header.purpose;
        held_prompt_digest_    = header.system_prompt_digest;
        held_tool_digests_     = header.tool_schema_digests;
        break;
    }
}

AgentLoop::~AgentLoop() = default;

AgentId AgentLoop::id() const {
    return id_;
}

SessionId AgentLoop::session() const {
    return session_.id();
}

bool AgentLoop::turnInFlight() const noexcept {
    switch (state_) {
        case AgentState::Thinking:
        case AgentState::CallingTool:
        case AgentState::WaitingForPermission:
        case AgentState::WaitingForInput:
        case AgentState::Cancelling:
            return true;
        case AgentState::Idle:
        case AgentState::Error:
            return false;
    }
    return false;
}

AgentStatus AgentLoop::status() const noexcept {
    return turnInFlight() ? AgentStatus::Running : AgentStatus::Idle;
}

AgentState AgentLoop::state() const noexcept {
    return state_;
}

bool AgentLoop::disposed() const noexcept {
    return disposed_;
}

bool AgentLoop::hasTurnTriggerLocked() const noexcept {
    for (const InboxItem& item : inbox_) {
        if (item.kind == InboxKind::Send || item.kind == InboxKind::FollowUp ||
            item.kind == InboxKind::Steer || item.kind == InboxKind::Compact) {
            return true;
        }
        if (item.kind == InboxKind::Inject && item.context.startsTurn) {
            return true;
        }
    }
    return pending_maintenance_failure_;
}

bool AgentLoop::hasTurnTrigger() const noexcept {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return hasTurnTriggerLocked();
}

bool AgentLoop::hasPendingWork() const noexcept {
    return hasTurnTrigger() || turnInFlight();
}

InboxResult AgentLoop::send(Message message) {
    InboxItem item;
    item.kind    = InboxKind::Send;
    item.origin  = running_ ? payload::TurnOrigin::FollowUp : payload::TurnOrigin::User;
    item.message = std::move(message);
    return enqueue(std::move(item));
}

InboxResult AgentLoop::followup(Message message) {
    InboxItem item;
    item.kind    = InboxKind::FollowUp;
    item.origin  = payload::TurnOrigin::FollowUp;
    item.message = std::move(message);
    return enqueue(std::move(item));
}

InboxResult AgentLoop::steer(Message message) {
    InboxItem item;
    item.kind    = InboxKind::Steer;
    item.origin  = payload::TurnOrigin::Steer;
    item.message = std::move(message);
    return enqueue(std::move(item));
}

InboxResult AgentLoop::inject(ContextMessage context) {
    InboxItem item;
    item.kind    = InboxKind::Inject;
    item.origin  = context.startsTurn ? payload::TurnOrigin::Injection : payload::TurnOrigin::User;
    item.context = std::move(context);
    return enqueue(std::move(item));
}

InboxResult AgentLoop::enqueue(InboxItem item) {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (disposed_) {
            return InboxResult::AgentDisposed;
        }
        if (inbox_.size() >= config_.max_inbox) {
            return InboxResult::InboxFull;
        }
        if (state_ == AgentState::Error) {
            state_ = AgentState::Idle;
        }
        inbox_.push_back(std::move(item));
    }
    if (!running_) {
        activate();
    }
    return InboxResult::Accepted;
}

void AgentLoop::activate() {
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (disposed_ || running_) {
            return;
        }
        running_ = true;
    }
    while (!disposed_ && hasTurnTrigger()) {
        bool maintenance = false;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            maintenance = pending_maintenance_failure_;
            if (maintenance) {
                pending_maintenance_failure_ = false;
            }
        }
        if (maintenance) {
            const TurnId turn = session_.nextTurnId();
            session_.append(payload::TurnStarted{turn, payload::TurnOrigin::Maintenance});
            appendTurnFailed(turn, AgentErrorCode::InboxFull,
                             "compaction request rejected: inbox full");
            continue;
        }
        if (state_ == AgentState::Error) {
            break;
        }
        runTurn();
    }
    running_ = false;
    if (state_ != AgentState::Error) {
        state_ = AgentState::Idle;
    }
    flushSettleCallbacks();
    flushIdleCallbacks();
}

void AgentLoop::cancel() {
    if (disposed_ || !turnInFlight() || state_ == AgentState::Cancelling) {
        return;
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    cancel_reason_ = "user";
    state_        = AgentState::Cancelling;
    turn_cancel_.cancel();
}

void AgentLoop::suspend() {
    if (disposed_ || !turnInFlight() || state_ == AgentState::Cancelling) {
        return;
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    cancel_reason_ = "superseded";
    state_        = AgentState::Cancelling;
    turn_cancel_.cancel();
}

std::expected<CompactionOutcome, AgentError> AgentLoop::requestCompaction() {
    if (disposed_) {
        return std::unexpected(AgentError{AgentErrorCode::AgentDisposed, "agent disposed"});
    }
    bool inbox_full = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        inbox_full = inbox_.size() >= config_.max_inbox;
        if (inbox_full) {
            pending_maintenance_failure_ = true;
        }
    }
    if (inbox_full) {
        if (!running_) {
            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                if (state_ == AgentState::Error) {
                    state_ = AgentState::Idle;
                }
            }
            activate();
        }
        return std::unexpected(AgentError{AgentErrorCode::InboxFull, "inbox full"});
    }
    InboxItem item;
    item.kind   = InboxKind::Compact;
    item.origin = payload::TurnOrigin::Maintenance;
    const InboxResult queued = enqueue(std::move(item));
    if (queued == InboxResult::Accepted) {
        return CompactionOutcome::Queued;
    }
    if (queued == InboxResult::AgentDisposed) {
        return std::unexpected(AgentError{AgentErrorCode::AgentDisposed, "agent disposed"});
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        pending_maintenance_failure_ = true;
    }
    if (!running_) {
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            if (state_ == AgentState::Error) {
                state_ = AgentState::Idle;
            }
        }
        activate();
    }
    return std::unexpected(AgentError{AgentErrorCode::InboxFull, "inbox full"});
}

void AgentLoop::dispose() {
    if (disposed_.exchange(true)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (turnInFlight()) {
            cancel_reason_ = "user";
            state_        = AgentState::Cancelling;
            turn_cancel_.cancel();
        }
        inbox_.clear();
        pending_maintenance_failure_ = false;
        settle_callbacks_.clear();
    }
    state_   = AgentState::Idle;
    running_ = false;
    flushIdleCallbacks();
}

void AgentLoop::whenIdle(std::function<void()> callback) {
    if (!callback) {
        return;
    }
    bool run_now = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (!running_ && !hasTurnTriggerLocked() && !turnInFlight()) {
            run_now = true;
        } else {
            idle_callbacks_.push_back(std::move(callback));
        }
    }
    if (run_now) {
        callback();
    }
}

void AgentLoop::flushIdleCallbacks() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (running_ || hasTurnTriggerLocked() || turnInFlight()) {
            return;
        }
        callbacks = std::move(idle_callbacks_);
        idle_callbacks_.clear();
    }
    for (auto& callback : callbacks) {
        callback();
    }
}

void AgentLoop::onSettled(std::function<void()> callback) {
    if (!callback) {
        return;
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (disposed_) {
        return;
    }
    settle_callbacks_.emplace_back(settle_generation_, std::move(callback));
}

void AgentLoop::noteTerminal() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    ++settle_generation_;
}

void AgentLoop::flushSettleCallbacks() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (running_ || hasTurnTriggerLocked() || turnInFlight()) {
            return;
        }
        for (auto it = settle_callbacks_.begin(); it != settle_callbacks_.end();) {
            if (it->first < settle_generation_) {
                callbacks.push_back(std::move(it->second));
                it = settle_callbacks_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& callback : callbacks) {
        callback();
    }
}

Task<void> AgentLoop::run(CancellationToken sessionCancel) {
    if (sessionCancel.cancelled()) {
        return Task<void>{};
    }
    sessionCancel.on_cancel([this]() { suspend(); });
    activate();
    return Task<void>{};
}

void AgentLoop::appendUserMessage(const Message& message) {
    payload::UserMessage user;
    user.id      = make_event_id().value;
    user.content = message.content;
    user.source  = message.source.value_or(message_source(MessageSource::Kind::User));
    session_.append(user);
}

void AgentLoop::appendContextInjected(const ContextMessage& context) {
    payload::ContextInjected injected;
    injected.id      = make_event_id().value;
    injected.role    = context.role;
    injected.text    = context.text;
    injected.source  = context.source;
    injected.context = context.context;
    if (injected.source.kind == MessageSource::Kind::Plugin) {
        if (injected.source.plugin.empty()) {
            injected.source.plugin = "agent";
        }
        injected.source.context = injected.context;
    }
    session_.append(injected);
}

const std::optional<std::string>& AgentLoop::active_scope() {
    if (!scope_resolved_) {
        scope_resolved_ = true;
        if (services_.presets != nullptr) {
            scope_ = services_.presets->scope_for(id_);
        }
    }
    return scope_;
}

void AgentLoop::materializeInstructions() {
    if (services_.instructions == nullptr || instructions_loaded_) {
        return;
    }
    instructions_loaded_ = true;
    const LoadedInstructions loaded = services_.instructions->load();
    const std::optional<std::string> message = loaded.render_message();
    if (!message.has_value()) {
        return;
    }
    ContextMessage context;
    context.role    = Role::User;
    context.text    = *message;
    context.context = ContextFormed{ContextForm::Instructions};
    context.source  = plugin_message_source("agent-instructions", context.context);
    appendContextInjected(context);
}

void AgentLoop::materializeContexts(const PromptAssembly& assembly) {
    const EventRange events = session_.events();
    for (const AssembledContext& context : assembly.contexts) {
        if (context.text.empty()) {
            continue;
        }
        const std::string identity = first_line(context.text);
        std::optional<std::string> previous;
        for (auto record = events.rbegin(); record != events.rend(); ++record) {
            if (record->event.type != EventType::ContextInjected) {
                continue;
            }
            const auto& injected = record->event.payload.get<payload::ContextInjected>();
            if (injected.role != Role::User || first_line(injected.text) != identity) {
                continue;
            }
            previous = injected.text;
            break;
        }
        if (previous.has_value() && *previous == context.text) {
            continue;
        }
        ContextMessage message;
        message.role    = Role::User;
        message.text    = context.text;
        message.context = ContextFormed{
            ContextForm::Snapshot,
            std::vector<ContextSnapshotSection>{{context.name, context.text}}};
        message.source = plugin_message_source("runtime-context", message.context);
        appendContextInjected(message);
    }
}

void AgentLoop::appendTurnFailed(TurnId turn, AgentErrorCode code, std::string message,
                                 bool recoverable) {
    payload::TurnFailed failed;
    failed.turn    = turn;
    failed.code    = std::string{agent_error_code_name(code)};
    failed.message = std::move(message);
    session_.append(failed);
    state_ = recoverable ? AgentState::Idle : AgentState::Error;
    noteTerminal();
}

CompactionOutcome AgentLoop::commitCompactionResult(const CompactionResult& result, TurnId turn) {
    if (result.outcome == CompactionOutcome::Compacted && result.compaction.has_value()) {
        session_.append(*result.compaction);
        ++compactions_this_turn_;
        if (result.usage.has_value()) {
            session_.append(payload::TokenUsage{*result.usage, turn});
        }
    }
    return result.outcome;
}

CompactionOutcome AgentLoop::runCompaction(const std::vector<Message>& messages, TurnId turn,
                                           CompactionTrigger trigger) {
    CancellationToken turnToken;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turnToken = turn_cancel_.token();
    }
    if (services_.context_compactor != nullptr) {
        std::optional<CompactionResult> result =
            services_.context_compactor->compact_if_needed(trigger, session_, messages, turnToken)
                .get();
        if (!result.has_value()) {
            return CompactionOutcome::NotNeeded;
        }
        return commitCompactionResult(*result, turn);
    }
    if (services_.compactor != nullptr) {
        std::optional<payload::ContextCompaction> compaction =
            services_.compactor->run(session_, messages, turnToken);
        if (compaction.has_value()) {
            session_.append(*compaction);
            ++compactions_this_turn_;
            return CompactionOutcome::Compacted;
        }
    }
    return CompactionOutcome::NotNeeded;
}

CompactionOutcome AgentLoop::runCompactionNow(const std::vector<Message>& messages, TurnId turn) {
    CancellationToken turnToken;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turnToken = turn_cancel_.token();
    }
    if (services_.context_compactor != nullptr) {
        return commitCompactionResult(
            services_.context_compactor->compact_now(session_, messages, turnToken).get(), turn);
    }
    if (services_.compactor != nullptr) {
        std::optional<payload::ContextCompaction> compaction =
            services_.compactor->run(session_, messages, turnToken);
        if (compaction.has_value()) {
            session_.append(*compaction);
            ++compactions_this_turn_;
            return CompactionOutcome::Compacted;
        }
    }
    return CompactionOutcome::NotNeeded;
}

ModelSelection AgentLoop::effective_model_selection() const {
    const ModelSelection configured{config_.model, config_.model_name, config_.endpoint,
                                    config_.parameters, config_.profile, config_.provider};
    if (services_.model_selection == nullptr) {
        return configured;
    }
    if (const std::optional<ModelSelection> selected =
            services_.model_selection->effective(session_);
        selected.has_value()) {
        return *selected;
    }
    // No durable selection: keep the session's OWN model (the folded header wire
    // id) with the configured parameters/profile. Never `config_.model`, which
    // can differ from the session's model on resume/fork.
    ModelSelection own = configured;
    if (!session_.header().model.empty()) {
        own.model = session_.header().model;
    }
    return own;
}

FrozenRequest AgentLoop::buildRequest(const std::vector<Message>& messages,
                                      TurnId turn, StepId step, std::size_t turn_step) {
    (void)pruner_.prune_session(session_);
    const ModelSelection selected = effective_model_selection();
    LLMRequest request;
    request.model    = selected.model;
    request.messages = messages;
    if (services_.context != nullptr) {
        request.tools = services_.context->tools(active_scope());
    }
    request.parameters = selected.parameters;
    request.session_id = session_.id();

    if (selected.profile.force_first_tool_call && turn_step == 1 && !request.tools.empty() &&
        !request.parameters.tool_choice.has_value()) {
        request.parameters.tool_choice = std::string{"required"};
    }

    LlmCallConfig config;
    config.provider         = selected.provider;
    config.endpoint         = selected.endpoint;
    config.profile_id       = selected.profile.id;
    config.model            = selected.model;
    config.reasoning_effort = selected.parameters.reasoning_effort;
    config.temperature      = selected.parameters.temperature;
    config.max_tokens       = selected.parameters.max_output_tokens;
    config.stop             = selected.parameters.stop;
    config.top_p            = selected.parameters.top_p;
    config.top_k            = selected.parameters.top_k;
    config.seed             = selected.parameters.seed;
    config.tool_choice      = request.parameters.tool_choice;

    FrozenRequest     frozen          = FrozenRequest::freeze(std::move(request), config);
    const std::string template_digest = frozen.template_digest();
    const std::string system_text     = assembled_system_prompt(frozen.get());
    const std::string prompt_digest   = sha256_hex(system_text);

    std::vector<std::string> tool_digests;
    tool_digests.reserve(frozen.get().tools.size());
    for (const ToolSchema& schema : frozen.get().tools) {
        tool_digests.push_back(tool_schema_digest(schema));
    }

    const bool config_changed =
        !held_config_.has_value() || !call_config_equals(config, *held_config_);
    const bool purpose_changed = held_purpose_ != frozen.get().purpose;
    const bool prompt_changed =
        !held_prompt_digest_.has_value() || prompt_digest != *held_prompt_digest_;
    const bool tools_changed =
        !held_tool_digests_.has_value() || tool_digests != *held_tool_digests_;

    if (config_changed || purpose_changed || prompt_changed || tools_changed) {
        payload::LlmRequestHeader header;
        header.turn       = turn;
        header.step       = step;
        header.session_id = frozen.get().session_id;
        header.purpose    = frozen.get().purpose;
        header.config     = config;

        header.system_prompt_digest = prompt_digest;
        if (config_.persist_prompt_text) {
            header.system_prompt = system_text;
        }
        for (const ToolSchema& schema : frozen.get().tools) {
            header.tool_names.push_back(schema.name.value);
        }
        header.tool_schema_digests = tool_digests;
        header.template_digest     = template_digest;
        // 28 §5.1 / A21: a purpose-only change is not a KV-cache boundary.
        header.starts_series = config_changed || prompt_changed || tools_changed;
        session_.append(header);
    }

    held_config_        = config;
    held_purpose_       = frozen.get().purpose;
    held_prompt_digest_ = prompt_digest;
    held_tool_digests_  = tool_digests;
    return frozen;
}

void AgentLoop::drainFoldedItems() {
    std::vector<InboxItem> folded;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        while (!inbox_.empty()) {
            InboxItem& item = inbox_.front();
            if (item.kind == InboxKind::Steer) {
                folded.push_back(std::move(item));
                inbox_.pop_front();
            } else if (item.kind == InboxKind::Inject && !item.context.startsTurn) {
                folded.push_back(std::move(item));
                inbox_.pop_front();
            } else {
                break;
            }
        }
    }
    for (InboxItem& item : folded) {
        if (item.kind == InboxKind::Steer) {
            appendUserMessage(item.message);
        } else {
            appendContextInjected(item.context);
        }
    }
}

AgentLoop::PreparedToolCall AgentLoop::prepareToolCall(const ToolCallAssembled& assembled,
                                                       TurnId turn, StepId step) {
    PreparedToolCall   plan;
    payload::ToolCall& call = plan.call;
    CancellationToken  turnToken;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turnToken = turn_cancel_.token();
    }
    plan.token       = turnToken;
    call.id          = assembled.id;
    call.turn        = turn;
    call.step        = step;
    call.name        = assembled.name;
    call.arguments   = assembled.arguments;
    call.requestedAt = std::chrono::system_clock::now();
    session_.append(call);

    const auto reject = [&](std::string message) {
        payload::ToolResult rejected;
        rejected.id      = call.id;
        rejected.name    = call.name;
        rejected.outcome = payload::ToolOutcome::Error;
        rejected.output  = std::move(message);
        rejected.error   = rejected.output;
        rejected.source.call = rejected.id;
        plan.run     = false;
        plan.decided = std::move(rejected);
    };

    const bool plan_active =
        services_.plan_mode != nullptr && services_.plan_mode->active(session_);
    if (call.name == "exit_plan_mode") {
        if (!plan_active) {
            reject("exit_plan_mode is only valid in plan mode");
            return plan;
        }
        if (!has_valid_plan_argument(call.arguments)) {
            reject("exit_plan_mode requires a non-empty string 'plan' argument");
            return plan;
        }
    }

    PermissionRequest request;
    request.call      = call.id;
    request.session   = session_.id();
    request.turn      = turn;
    request.step      = step;
    request.tool      = call.name;
    request.arguments = call.arguments;
    if (services_.execution != nullptr) {
        request.root = services_.execution->root();
    }
    request.sandbox = config_.sandbox;
    if (services_.tools != nullptr) {
        if (Tool* tool = services_.tools->find(ToolName{call.name}); tool != nullptr) {
            request.destructive = tool->schema().destructive;
        }
    }
    if (call.name == "exit_plan_mode") {
        request.force_ask = true;
    }

    payload::PermissionDecisionKind decision = payload::PermissionDecisionKind::Deny;
    std::string                     reason;

    if (services_.gate != nullptr) {
        std::weak_ptr<Session> weak = session_owner_;
        services_.gate->set_decision_hook(
            [weak](const payload::PermissionDecision& recorded) {
                if (auto session = weak.lock()) {
                    session->append(recorded);
                }
            });
        struct HookClear {
            PermissionGate* gate;
            ~HookClear() { gate->set_decision_hook({}); }
        } clear{services_.gate};
        state_ = AgentState::WaitingForPermission;
        const PermissionOutcome outcome = services_.gate->resolve(request, turnToken);
        decision = outcome.decision;
        reason   = outcome.reason;
        state_   = AgentState::CallingTool;
    } else {
        const PolicyVerdict verdict =
            services_.policy != nullptr ? services_.policy->evaluate(request) : PolicyVerdict::Ask;
        if (verdict == PolicyVerdict::Allow) {
            decision = payload::PermissionDecisionKind::Allow;
        } else if (verdict == PolicyVerdict::Deny) {
            decision = payload::PermissionDecisionKind::Deny;
            reason   = "denied by policy";
        } else {
            state_ = AgentState::WaitingForPermission;
            if (services_.permission_resolver) {
                const PermissionOutcome outcome =
                    services_.permission_resolver(request, turnToken);
                decision = outcome.decision;
                reason   = outcome.reason;
            } else {
                decision = payload::PermissionDecisionKind::Deny;
                reason   = request.force_ask ? "exit_plan_mode requires an interactive review"
                                             : "no permission resolver attached";
            }
            state_ = AgentState::CallingTool;
        }

        payload::PermissionDecision recorded;
        recorded.call     = call.id;
        recorded.decision = decision;
        recorded.reason   = reason;
        session_.append(recorded);

        if (services_.policy != nullptr && !request.force_ask) {
            const GrantScope scope = decision == payload::PermissionDecisionKind::AllowAlways
                                         ? GrantScope::Always
                                         : GrantScope::Once;
            services_.policy->remember(request, decision, scope);
        }
    }

    plan.decision = decision;
    if (decision == payload::PermissionDecisionKind::Deny) {
        payload::ToolResult denied;
        denied.id      = call.id;
        denied.name    = call.name;
        denied.outcome = payload::ToolOutcome::Denied;
        denied.output  = reason.empty() ? std::string{"permission denied"} : reason;
        denied.error   = denied.output;
        denied.source.call = denied.id;
        plan.run     = false;
        plan.decided = std::move(denied);
    }
    return plan;
}

payload::ToolResult AgentLoop::runToolCall(const PreparedToolCall& plan) {
    payload::ToolResult result;
    if (services_.tools == nullptr || services_.execution == nullptr ||
        services_.logger == nullptr || services_.governor == nullptr ||
        services_.output == nullptr) {
        result.id      = plan.call.id;
        result.name    = plan.call.name;
        result.outcome = payload::ToolOutcome::Error;
        result.output  = "tool runtime unavailable";
        result.error   = result.output;
        return result;
    }

    StaticPermissionHandle handle(plan.decision);
    std::optional<std::chrono::steady_clock::time_point> deadline;
    const std::chrono::milliseconds tool_timeout =
        services_.execution->toolConfig().tool_timeout;
    if (tool_timeout.count() > 0) {
        deadline = std::chrono::steady_clock::now() + tool_timeout;
    }
    ToolContext context(*services_.execution, session_, *services_.logger, plan.token,
                        *services_.governor, *services_.output, handle, plan.call.id,
                        plan.call.turn, plan.call.step, deadline);
    try {
        result = services_.tools->execute(plan.call, context).get();
    } catch (const std::exception& error) {
        result.id      = plan.call.id;
        result.name    = plan.call.name;
        result.outcome = payload::ToolOutcome::Error;
        result.output  = error.what();
        result.error   = std::string{error.what()};
    }
    return result;
}

payload::ToolResult AgentLoop::commitToolResult(const PreparedToolCall& plan,
                                                payload::ToolResult       result) {
    if (result.id.empty()) {
        result.id = plan.call.id;
    }
    if (result.name.empty()) {
        result.name = plan.call.name;
    }
    result.source.call = result.id;
    session_.append(result);
    if (plan.call.name == "exit_plan_mode" && result.outcome == payload::ToolOutcome::Ok &&
        services_.plan_mode != nullptr) {
        services_.plan_mode->request_exit(session_.id());
    }
    return result;
}

ToolConcurrencyMode AgentLoop::toolConcurrencyMode(const std::string& name) const {
    if (services_.tools == nullptr) {
        return ToolConcurrencyMode::Exclusive;
    }
    Tool* tool = services_.tools->find(ToolName{name});
    return tool == nullptr ? ToolConcurrencyMode::Exclusive : tool->schema().concurrency;
}

bool AgentLoop::executeToolCall(const ToolCallAssembled& assembled, TurnId turn, StepId step,
                                payload::ToolResult* out) {
    PreparedToolCall    plan = prepareToolCall(assembled, turn, step);
    payload::ToolResult result = plan.run ? runToolCall(plan) : plan.decided;
    result                     = commitToolResult(plan, std::move(result));
    if (out != nullptr) {
        *out = result;
    }
    return plan.run;
}

void AgentLoop::runMaintenanceTurn(TurnId turn) {
    session_.append(payload::TurnStarted{turn, payload::TurnOrigin::Maintenance});
    TurnFlushGuard turn_flush{services_.plan_mode, services_.model_selection, session_};
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turn_cancel_   = CancellationSource{};
        cancel_reason_ = "user";
    }
    compactions_this_turn_ = 0;

    const StepId step = session_.nextStepId();
    session_.append(payload::StepStarted{turn, step});
    if (services_.plan_mode != nullptr) {
        services_.plan_mode->apply_pending_at_step_start(session_);
    }
    if (services_.model_selection != nullptr) {
        services_.model_selection->apply_pending_at_step_start(session_);
    }
    state_ = AgentState::Thinking;

    if (services_.context == nullptr) {
        appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, "no context assembler");
        return;
    }
    std::vector<Message> messages;
    try {
        (void)pruner_.prune_session(session_);
        messages =
            services_.context->assemble(session_, TurnContext{turn, step, {}, {}, active_scope()});
    } catch (const std::exception& error) {
        appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, error.what());
        return;
    }

    const CompactionOutcome outcome = runCompactionNow(messages, turn);
    if (outcome == CompactionOutcome::Cancelled) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            reason = cancel_reason_.empty() ? std::string{"user"} : cancel_reason_;
        }
        session_.append(payload::TurnCancelled{turn, reason});
        state_ = AgentState::Idle;
        noteTerminal();
        return;
    }
    if (outcome == CompactionOutcome::Failed) {
        appendTurnFailed(turn, AgentErrorCode::CompactionFailed, "compaction failed");
        return;
    }
    session_.append(payload::StepEnded{turn, step});
    session_.append(payload::TurnEnded{turn, std::nullopt});
    state_ = AgentState::Idle;
    noteTerminal();
}

void AgentLoop::note_answer_observation(const std::string& prompt_hash,
                                        const std::string& answer_hash) {
    recent_answers_.push_back(AnswerObservation{prompt_hash, answer_hash});
    while (recent_answers_.size() > kAnswerLogCapacity) {
        recent_answers_.pop_front();
    }
}

bool AgentLoop::repeats_prior_answer(const std::string& prompt_hash,
                                     const std::string& answer_hash) const {
    if (answer_hash.empty()) {
        return false;
    }
    std::unordered_set<std::string> distinct_prompts;
    distinct_prompts.insert(prompt_hash);
    for (const AnswerObservation& observation : recent_answers_) {
        if (observation.answer_hash == answer_hash) {
            distinct_prompts.insert(observation.prompt_hash);
        }
    }
    return distinct_prompts.size() >= kRepeatOccurrences;
}

bool AgentLoop::repeats_same_prompt_answer(const std::string& prompt_hash,
                                           const std::string& answer_hash) const {
    if (answer_hash.empty()) {
        return false;
    }
    std::size_t occurrences = 0;
    for (const AnswerObservation& observation : recent_answers_) {
        if (observation.prompt_hash == prompt_hash && observation.answer_hash == answer_hash) {
            ++occurrences;
        }
    }
    return occurrences >= kSamePromptRepeatOccurrences;
}

void AgentLoop::runTurn() {
    drainFoldedItems();
    InboxItem trigger;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (inbox_.empty()) {
            return;
        }
        trigger = std::move(inbox_.front());
        inbox_.pop_front();
    }

    const TurnId turn = session_.nextTurnId();
    if (trigger.kind == InboxKind::Compact) {
        runMaintenanceTurn(turn);
        return;
    }
    if (trigger.kind == InboxKind::Inject) {
        appendContextInjected(trigger.context);
    } else {
        // 36 §2.7: the durable prefix (workspace instructions, then the
        // runtime-context snapshot) is materialized before the user prompt so
        // the prompt is the last user-role message in the assembled request.
        // The per-step `assemble_messages` re-materialization stays for the
        // changed-snapshot refresh; `materializeContexts` dedups identical text.
        materializeInstructions();
        if (services_.prompt != nullptr) {
            materializeContexts(
                services_.prompt->assemble(AssembleContext{.scope = active_scope()}));
        }
        appendUserMessage(trigger.message);
    }
    // 73-D9: the normalized current user prompt, captured once for the turn.
    // Repetition is only meaningful for a turn that carries user prompt text, so
    // an Inject turn (or an empty/whitespace-only prompt) is ineligible: it
    // neither records nor matches, which keeps non-user prompts out of the
    // distinct-prompt count.
    const std::string normalized_prompt = normalize_reply(text_of_blocks(trigger.message.content));
    const bool        repetition_eligible = !normalized_prompt.empty();
    const std::string current_prompt_hash = sha256_hex(normalized_prompt);
    session_.append(payload::TurnStarted{turn, trigger.origin});
    TurnFlushGuard turn_flush{services_.plan_mode, services_.model_selection, session_};

    CancellationToken turnToken;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turn_cancel_   = CancellationSource{};
        cancel_reason_ = "user";
        turnToken      = turn_cancel_.token();
    }
    compactions_this_turn_ = 0;

    const auto assemble_messages = [&](StepId step) -> std::vector<Message> {
        (void)pruner_.prune_session(session_);
        const std::optional<std::string>& scope = active_scope();
        if (services_.prompt != nullptr) {
            materializeContexts(services_.prompt->assemble(AssembleContext{.scope = scope}));
        }
        return services_.context->assemble(session_, TurnContext{turn, step, {}, {}, scope});
    };

    const auto compaction_available = [&]() -> bool {
        if (services_.context_compactor != nullptr) {
            return compactions_this_turn_ <
                   services_.context_compactor->policy().max_compactions_per_turn;
        }
        return services_.compactor != nullptr && compactions_this_turn_ < 1;
    };
    const bool threshold_enabled =
        services_.context_compactor != nullptr
            ? services_.context_compactor->policy().is_enabled()
            : config_.compaction_threshold_tokens > 0;

    // 62-D3/D8 (Rev 3): `max_steps` is the per-SEGMENT budget (default 100),
    // clamped to >= 1. A segment that commits a successful tool call
    // auto-continues into a new segment with no user message; a segment with no
    // success stops at the budget. `max_segments == 0` means no cap on segments.
    // 62-D9 (Rev 4): `max_turn_steps` is the ABSOLUTE per-turn ceiling, checked
    // before any work each iteration, so a novel-`Ok` runaway (invisible to the
    // no-progress guard) is bounded even when `max_segments == 0`.
    const std::size_t step_ceiling = std::max<std::size_t>(1, config_.max_steps);
    const std::size_t turn_ceiling = std::max<std::size_t>(1, config_.max_turn_steps);
    const std::size_t segment_cap = config_.max_segments == 0
                                        ? std::numeric_limits<std::size_t>::max()
                                        : config_.max_segments;
    std::size_t       segment_step         = 0;
    std::size_t       segments_completed   = 0;
    std::size_t       no_progress_streak   = 0;
    bool              segment_had_success  = false;
    // 73-D3: at most one corrective nudge per user turn; reset for every
    // runTurn(), so a second non-action cannot re-enter the nudge branch.
    bool nudge_emitted_for_turn = false;
    // 73-D10: at most one repetition observation per user turn.
    bool answer_observed_for_turn = false;
    std::unordered_set<std::string> seen_actions;
    std::unordered_set<std::string> seen_results;
    for (std::size_t stepNumber = 1;; ++stepNumber) {
        if (stepNumber > turn_ceiling) {
            appendTurnFailed(
                turn, AgentErrorCode::StepLimitExceeded,
                "turn step ceiling (" + std::to_string(turn_ceiling) +
                    ") reached \u2014 task incomplete; send a message to continue",
                /*recoverable=*/true);
            return;
        }
        const StepId step = session_.nextStepId();
        session_.append(payload::StepStarted{turn, step});
        if (services_.plan_mode != nullptr) {
            services_.plan_mode->apply_pending_at_step_start(session_);
        }
        if (services_.model_selection != nullptr) {
            services_.model_selection->apply_pending_at_step_start(session_);
        }

        drainFoldedItems();
        state_ = AgentState::Thinking;

        if (services_.context == nullptr) {
            appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, "no context assembler");
            return;
        }

        std::vector<Message> messages;
        try {
            materializeInstructions();
            messages = assemble_messages(step);
        } catch (const std::exception& error) {
            appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, error.what());
            return;
        }
        if (services_.estimator != nullptr && compaction_available() && threshold_enabled) {
            const std::size_t estimate = services_.estimator->estimate(messages);
            const std::size_t threshold = services_.context_compactor != nullptr
                                              ? services_.context_compactor->policy()
                                                    .effective_threshold_tokens()
                                              : config_.compaction_threshold_tokens;
            if (estimate > threshold) {
                runCompaction(messages, turn, CompactionTrigger::Pressure);
                try {
                    messages = assemble_messages(step);
                } catch (const std::exception& error) {
                    appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, error.what());
                    return;
                }
            }
        }

        std::optional<FrozenRequest> request;
        request.emplace(buildRequest(messages, turn, step, stepNumber));

        const MessageId      messageId = make_event_id().value;
        std::optional<Usage> settledUsage;
        // 73-D1: settled text of the Completed attempt, captured outside the
        // per-attempt scope so the terminal decision can inspect it.
        std::string settled_text;

        LLMResponse response;
        bool        slotCancelled = false;
        if (services_.runtime == nullptr) {
            appendTurnFailed(turn, AgentErrorCode::ProviderFailed, "no provider configured");
            return;
        }

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (services_.pool != nullptr) {
                std::optional<LLMPool::Slot> slot =
                    services_.pool->acquire(turnToken).get();
                if (!slot.has_value()) {
                    slotCancelled = true;
                    break;
                }
            }

            // 34-D5: a fresh assembler/accumulator/coalescer and stream_start
            // per provider attempt; a retried attempt must not leak into the
            // retry's durable message.
            BlockAssembler             assembler;
            AssistantStreamAccumulator accumulator;
            ChunkCoalescer             coalescer(session_, messageId, config_.max_chunk_batch,
                                                 config_.chunk_flush_interval);
            // 34 §5.1: the epoch is the instant immediately before this
            // attempt's `call.stream(...)`, not before `prepare_call`.
            AgentServices::StreamClock::time_point stream_start{};

            StreamSink sink = [&](const StreamEvent& event) -> SinkFlow {
                const TimedStreamEvent timed{
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        services_.stream_clock() - stream_start),
                    event};
                assembler.push(event);
                const TimedStreamEvent live = accumulator.push(timed);
                if (const auto* delta = std::get_if<TextDelta>(&live.event)) {
                    coalescer.onText(delta->text);
                } else if (const auto* delta = std::get_if<ReasoningDelta>(&live.event)) {
                    coalescer.onReasoning(delta->text);
                }
                return SinkFlow::Continue;
            };

            bool dispatched = false;
            try {
                PreparedCall call =
                    services_.runtime->prepare_call(request->config(), turnToken).get();
                stream_start = services_.stream_clock();
                response = call.stream(std::move(*request), sink, turnToken).get();
                dispatched = true;
            } catch (const NoProviderRouteError& error) {
                response              = LLMResponse{};
                response.outcome      = StreamOutcome::Failed;
                response.finish       = FinishReason::Error;
                response.error.code   = LLMErrorCode::NoProviderRoute;
                response.error.detail = error.detail;
            } catch (const PreparedCallError& error) {
                response              = LLMResponse{};
                response.outcome      = StreamOutcome::Failed;
                response.finish       = FinishReason::Error;
                response.error.code   = LLMErrorCode::InvalidPreparedCall;
                response.error.detail = error.detail;
            } catch (const std::exception& error) {
                response              = LLMResponse{};
                response.outcome      = StreamOutcome::Failed;
                response.finish       = FinishReason::Error;
                response.error.code   = LLMErrorCode::ProviderInternal;
                response.error.detail = error.what();
            }

            // 34 §7.3: settlement happens only at the return of a real
            // `call.stream(...)`. A `prepare_call` failure never dispatched a
            // provider call, so it does not settle an attempt.
            if (!dispatched) {
                break;
            }

            // 34-D6: settle this attempt — stream -> flush -> settle. Exactly
            // one durable event per settled attempt. `flush` is a live-only
            // `Session::emit` (29-D4) and cannot throw LeaseLost/StoreError; a
            // throw here can only come from a bus subscriber.
            try {
                coalescer.flush();
            } catch (const std::exception& error) {
                appendTurnFailed(turn, AgentErrorCode::Internal, error.what());
                return;
            }

            if (response.outcome == StreamOutcome::Completed) {
                payload::AssistantMessage assistant;
                assistant.id           = messageId;
                assistant.content      = assembler.blocks();
                assistant.usage        = assembler.usage();
                assistant.stream       = accumulator.snapshot();
                assistant.replay_state = assembler.replay_state();
                const ModelSelection stamped = effective_model_selection();
                assistant.source = model_message_source(stamped.provider, stamped.model);
                settled_text = text_of_blocks(assistant.content);
                session_.append(assistant);
                settledUsage = assembler.usage();
            } else {
                payload::AssistantAttempt attemptEvent;
                attemptEvent.turn   = turn;
                attemptEvent.step   = step;
                attemptEvent.stream = accumulator.snapshot();
                session_.append(attemptEvent);
            }

            if (response.outcome != StreamOutcome::Failed ||
                response.error.code != LLMErrorCode::ContextLengthExceeded) {
                break;
            }
            if (attempt == 1) {
                break;
            }
            if (!compaction_available()) {
                break;
            }
            if (services_.context_compactor != nullptr &&
                !services_.context_compactor->policy().retry_on_context_length) {
                break;
            }
            runCompaction(messages, turn, CompactionTrigger::ContextOverflow);
            try {
                messages = assemble_messages(step);
            } catch (const std::exception& error) {
                appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, error.what());
                return;
            }
            request.emplace(buildRequest(messages, turn, step, stepNumber));
        }
        if (slotCancelled) {
            response         = LLMResponse{};
            response.outcome = StreamOutcome::Cancelled;
            response.finish  = FinishReason::Other;
        }

        // 34-D11: the settled Completed attempt's `assembler.usage()` is the
        // single durable usage source.
        const std::optional<Usage>& usage = settledUsage;

        std::string cancelReason;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            cancelReason = cancel_reason_.empty() ? std::string{"user"} : cancel_reason_;
        }

        if (response.outcome == StreamOutcome::Cancelled) {
            session_.append(payload::TurnCancelled{turn, cancelReason});
            state_ = AgentState::Idle;
            noteTerminal();
            return;
        }
        if (response.outcome == StreamOutcome::Failed) {
            const std::string detail = response.error.detail.empty()
                                           ? response.error.provider_message
                                           : response.error.detail;
            appendTurnFailed(turn, mapAgentError(response.error.code), detail);
            return;
        }

        // 73-D1/D2/D4/D9/D11: a Completed response with no tool call is a
        // non-action when its settled text is blank, OR it repeats a prior turn's
        // answer to a different prompt (73-D9), OR the exact same normalized
        // `(prompt, answer)` pair has recurred (73-D11). The first non-action in
        // a turn earns exactly one bounded corrective nudge and re-enters the
        // step loop; a second one stops recoverably instead of looping. A
        // non-blank, non-repeated answer (73-D5) and tool calls are untouched.
        const bool no_tool = response.tool_calls.empty();
        const bool blank   = no_tool && is_blank(settled_text);
        std::string answer_hash;
        if (no_tool && !blank) {
            answer_hash = sha256_hex(normalize_reply(settled_text));
        }
        // 73-D10: record at the turn's first no-tool decision, whatever its
        // text; a blank first answer records nothing, but still consumes the
        // turn's single observation so the post-nudge answer is never recorded.
        if (no_tool && !answer_observed_for_turn) {
            answer_observed_for_turn = true;
            if (repetition_eligible && !answer_hash.empty()) {
                note_answer_observation(current_prompt_hash, answer_hash);
            }
        }
        const bool repeated_distinct =
            no_tool && repetition_eligible && repeats_prior_answer(current_prompt_hash, answer_hash);
        // 73-D11: the same normalized `(prompt, answer)` pair repeated across
        // turns is also a non-action; this is the verbatim-prompt case the
        // distinct-prompt arm above deliberately excludes.
        const bool repeated_same = no_tool && repetition_eligible &&
                                   repeats_same_prompt_answer(current_prompt_hash, answer_hash);
        const bool non_action = no_tool && (blank || repeated_distinct || repeated_same);
        if (non_action && !nudge_emitted_for_turn) {
            if (usage.has_value()) {
                session_.append(payload::TokenUsage{*usage, turn});
            }
            session_.append(payload::StepEnded{turn, step});
            nudge_emitted_for_turn = true;
            appendUserMessage(make_nudge_message());
            continue;
        }
        if (non_action) {
            if (usage.has_value()) {
                session_.append(payload::TokenUsage{*usage, turn});
            }
            session_.append(payload::StepEnded{turn, step});
            appendTurnFailed(turn, AgentErrorCode::StepLimitExceeded,
                             "no substantive response after corrective nudge "
                             "\u2014 task incomplete; send a message to continue",
                             /*recoverable=*/true);
            return;
        }
        if (response.tool_calls.empty()) {
            if (usage.has_value()) {
                session_.append(payload::TokenUsage{*usage, turn});
            }
            session_.append(payload::StepEnded{turn, step});
            session_.append(payload::TurnEnded{turn, response.finish});
            state_ = AgentState::Idle;
            noteTerminal();
            return;
        }

        state_ = AgentState::CallingTool;
        ContextAcceptor accept = [this](const ContextMessage& message) {
            appendContextInjected(message);
            return true;
        };
        ToolScheduleOutcome scheduled;
        try {
            scheduled = execute_tool_calls(*this, turn, step, response.tool_calls, turnToken,
                                           std::move(accept))
                            .get();
        } catch (const std::exception& error) {
            appendTurnFailed(turn, AgentErrorCode::Internal, error.what());
            return;
        } catch (...) {
            appendTurnFailed(turn, AgentErrorCode::Internal, "tool scheduler failure");
            return;
        }

        if (usage.has_value()) {
            session_.append(payload::TokenUsage{*usage, turn});
        }
        session_.append(payload::StepEnded{turn, step});

        if (turnToken.cancelled()) {
            session_.append(payload::TurnCancelled{turn, cancelReason});
            state_ = AgentState::Idle;
            return;
        }
        ++segment_step;
        const bool action_novel =
            seen_actions.insert(action_signature(response.tool_calls)).second;
        const bool result_novel =
            seen_results.insert(result_signature(scheduled)).second;
        const bool success = step_had_success(scheduled);
        segment_had_success = segment_had_success || success;
        const bool made_progress = action_novel || (success && result_novel);
        no_progress_streak = made_progress ? 0 : no_progress_streak + 1;
        if (no_progress_streak >= kNoProgressStreak) {
            appendTurnFailed(
                turn, AgentErrorCode::StepLimitExceeded,
                "no progress: " + std::to_string(kNoProgressStreak) +
                    " consecutive steps repeated the same action with no successful tool call "
                    "\u2014 task incomplete; send a message to continue",
                /*recoverable=*/true);
            return;
        }
        if (segment_step >= step_ceiling) {
            if (!segment_had_success) {
                appendTurnFailed(
                    turn, AgentErrorCode::StepLimitExceeded,
                    "step limit (" + std::to_string(step_ceiling) +
                        ") reached with no successful tool call \u2014 task incomplete; "
                        "send a message to continue",
                    /*recoverable=*/true);
                return;
            }
            ++segments_completed;
            if (segments_completed >= segment_cap) {
                appendTurnFailed(
                    turn, AgentErrorCode::StepLimitExceeded,
                    "step budget (" + std::to_string(segment_cap) +
                        " segments) exhausted \u2014 task incomplete; send a message to continue",
                    /*recoverable=*/true);
                return;
            }
            segment_step        = 0;
            segment_had_success = false;
        }
    }
}

Task<ToolScheduleOutcome> execute_tool_calls(AgentLoop& loop, TurnId turn, StepId step,
                                             std::vector<ToolCallAssembled> calls,
                                             CancellationToken cancel, ContextAcceptor accept) {
    ToolScheduleOutcome outcome;
    outcome.results.resize(calls.size());

    const std::size_t bound =
        std::max<std::size_t>(1, loop.config_.schedule.max_parallel_tool_calls);

    struct InFlight {
        std::size_t                      index;
        std::future<payload::ToolResult> future;
    };
    std::vector<InFlight>                    in_flight;
    std::vector<AgentLoop::PreparedToolCall> plans(calls.size());
    std::vector<bool>                        committed(calls.size(), false);

    const auto observe = [&](std::size_t index) {
        std::optional<ContextMessage> reminder = loop.reminder_.observe(calls[index]);
        if (!reminder.has_value()) {
            return;
        }
        if (accept && accept(*reminder)) {
            return;
        }
        if (loop.services_.logger != nullptr) {
            loop.services_.logger->warn("repeat-tool reminder injection rejected");
        }
    };

    const auto commit = [&](std::size_t index, payload::ToolResult result) {
        outcome.results[index] = loop.commitToolResult(plans[index], std::move(result));
        committed[index]       = true;
        observe(index);
    };

    const auto drain_all = [&]() {
        for (InFlight& item : in_flight) {
            commit(item.index, item.future.get());
        }
        in_flight.clear();
    };

    try {
        for (std::size_t index = 0; index < calls.size(); ++index) {
            if (cancel.cancelled()) {
                break;
            }

            if (loop.toolConcurrencyMode(calls[index].name) == ToolConcurrencyMode::Exclusive) {
                drain_all();
                if (cancel.cancelled()) {
                    break;
                }
                (void)loop.executeToolCall(calls[index], turn, step, &outcome.results[index]);
                committed[index] = true;
                observe(index);
                continue;
            }

            if (in_flight.size() >= bound) {
                InFlight oldest = std::move(in_flight.front());
                in_flight.erase(in_flight.begin());
                commit(oldest.index, oldest.future.get());
                if (cancel.cancelled()) {
                    break;
                }
            }

            plans[index] = loop.prepareToolCall(calls[index], turn, step);
            if (!plans[index].run) {
                drain_all();
                commit(index, plans[index].decided);
                if (cancel.cancelled()) {
                    break;
                }
                continue;
            }
            if (cancel.cancelled()) {
                break;
            }
            const AgentLoop::PreparedToolCall plan = plans[index];
            in_flight.push_back(InFlight{
                index, std::async(std::launch::async,
                                  [&loop, plan]() { return loop.runToolCall(plan); })});
        }
        drain_all();
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        for (InFlight& item : in_flight) {
            try {
                commit(item.index, item.future.get());
            } catch (...) {
            }
        }
        in_flight.clear();
        std::rethrow_exception(failure);
    }

    outcome.aborted = cancel.cancelled();
    if (outcome.aborted) {
        for (std::size_t index = 0; index < calls.size(); ++index) {
            if (committed[index]) {
                continue;
            }
            payload::ToolResult synthetic;
            synthetic.id          = calls[index].id;
            synthetic.name        = calls[index].name;
            synthetic.outcome     = payload::ToolOutcome::Cancelled;
            synthetic.output      = "tool call cancelled";
            synthetic.source.call = synthetic.id;
            loop.session_.append(synthetic);
            outcome.results[index] = std::move(synthetic);
            committed[index]       = true;
        }
    }

    return Task<ToolScheduleOutcome>(std::move(outcome));
}

} // namespace ymh

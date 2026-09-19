#include "ymh/agent/agent_loop.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ymh/execution/environment.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/tool.hpp"
#include "ymh/tools/tool_context.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh {
namespace {

ContentBlock text_block(ContentBlockKind kind, std::string text) {
    ContentBlock block;
    block.kind = kind;
    block.text = std::move(text);
    return block;
}

ContentBlock tool_use_block(const ToolCallAssembled& call) {
    ContentBlock block;
    block.kind         = ContentBlockKind::ToolUse;
    block.tool_call_id = call.id;
    block.tool_name    = call.name;
    block.arguments    = call.arguments;
    return block;
}

} // namespace

AgentLoop::AgentLoop(AgentId id, std::shared_ptr<Session> session, AgentServices services,
                     AgentConfig config)
    : id_(std::move(id)),
      session_owner_(std::move(session)),
      session_(*session_owner_),
      services_(std::move(services)),
      config_(std::move(config)) {}

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
    session_.append(user);
}

void AgentLoop::appendContextInjected(const ContextMessage& context) {
    payload::ContextInjected injected;
    injected.id   = make_event_id().value;
    injected.role = context.role;
    injected.text = context.text;
    session_.append(injected);
}

void AgentLoop::appendTurnFailed(TurnId turn, AgentErrorCode code, std::string message) {
    payload::TurnFailed failed;
    failed.turn    = turn;
    failed.code    = std::string{agent_error_code_name(code)};
    failed.message = std::move(message);
    session_.append(failed);
    state_ = AgentState::Error;
}

CompactionOutcome AgentLoop::runCompaction(const std::vector<Message>& messages, TurnId turn) {
    CancellationToken turnToken;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turnToken = turn_cancel_.token();
    }
    if (services_.context_compactor != nullptr) {
        CompactionResult result =
            services_.context_compactor->compact(session_, messages, turnToken);
        if (result.outcome == CompactionOutcome::Compacted && result.compaction.has_value()) {
            session_.append(*result.compaction);
            ++compactions_this_turn_;
            if (result.usage.has_value()) {
                session_.append(payload::TokenUsage{*result.usage, turn});
            }
        }
        return result.outcome;
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

LLMRequest AgentLoop::buildRequest(const std::vector<Message>& messages) const {
    LLMRequest request;
    request.model    = config_.model;
    request.messages = messages;
    if (services_.context != nullptr) {
        request.tools = services_.context->tools();
    }
    request.parameters = config_.parameters;
    return request;
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

bool AgentLoop::executeToolCall(const ToolCallAssembled& assembled, TurnId turn, StepId step) {
    CancellationToken turnToken;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turnToken = turn_cancel_.token();
    }
    payload::ToolCall call;
    call.id          = assembled.id;
    call.turn        = turn;
    call.step        = step;
    call.name        = assembled.name;
    call.arguments   = assembled.arguments;
    call.requestedAt = std::chrono::system_clock::now();
    session_.append(call);

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
                reason   = "no permission resolver attached";
            }
            state_ = AgentState::CallingTool;
        }

        payload::PermissionDecision recorded;
        recorded.call     = call.id;
        recorded.decision = decision;
        recorded.reason   = reason;
        session_.append(recorded);

        if (services_.policy != nullptr) {
            const GrantScope scope = decision == payload::PermissionDecisionKind::AllowAlways
                                         ? GrantScope::Always
                                         : GrantScope::Once;
            services_.policy->remember(request, decision, scope);
        }
    }

    if (decision == payload::PermissionDecisionKind::Deny) {
        payload::ToolResult denied;
        denied.id      = call.id;
        denied.name    = call.name;
        denied.outcome = payload::ToolOutcome::Denied;
        denied.output  = reason.empty() ? std::string{"permission denied"} : reason;
        denied.error   = denied.output;
        session_.append(denied);
        return false;
    }

    payload::ToolResult result;
    if (services_.tools == nullptr || services_.execution == nullptr ||
        services_.logger == nullptr || services_.governor == nullptr ||
        services_.output == nullptr) {
        result.id      = call.id;
        result.name    = call.name;
        result.outcome = payload::ToolOutcome::Error;
        result.output  = "tool runtime unavailable";
        result.error   = result.output;
        session_.append(result);
        return false;
    }

    StaticPermissionHandle handle(decision);
    ToolContext context(*services_.execution, session_, *services_.logger, turnToken,
                        *services_.governor, *services_.output, handle, call.id, turn, step);
    try {
        result = services_.tools->execute(call, context).get();
    } catch (const std::exception& error) {
        result.id      = call.id;
        result.name    = call.name;
        result.outcome = payload::ToolOutcome::Error;
        result.output  = error.what();
        result.error   = std::string{error.what()};
    }
    session_.append(result);
    return true;
}

void AgentLoop::runMaintenanceTurn(TurnId turn) {
    session_.append(payload::TurnStarted{turn, payload::TurnOrigin::Maintenance});
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turn_cancel_   = CancellationSource{};
        cancel_reason_ = "user";
    }
    compactions_this_turn_ = 0;

    const StepId step = session_.nextStepId();
    session_.append(payload::StepStarted{turn, step});
    state_ = AgentState::Thinking;

    if (services_.context == nullptr) {
        appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, "no context assembler");
        return;
    }
    std::vector<Message> messages;
    try {
        messages = services_.context->assemble(session_, TurnContext{turn, step, {}, {}});
    } catch (const std::exception& error) {
        appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, error.what());
        return;
    }

    const CompactionOutcome outcome = runCompaction(messages, turn);
    if (outcome == CompactionOutcome::Cancelled) {
        std::string reason;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            reason = cancel_reason_.empty() ? std::string{"user"} : cancel_reason_;
        }
        session_.append(payload::TurnCancelled{turn, reason});
        state_ = AgentState::Idle;
        return;
    }
    if (outcome == CompactionOutcome::Failed) {
        appendTurnFailed(turn, AgentErrorCode::CompactionFailed, "compaction failed");
        return;
    }
    session_.append(payload::StepEnded{turn, step});
    session_.append(payload::TurnEnded{turn});
    state_ = AgentState::Idle;
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
        appendUserMessage(trigger.message);
    }
    session_.append(payload::TurnStarted{turn, trigger.origin});

    CancellationToken turnToken;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        turn_cancel_   = CancellationSource{};
        cancel_reason_ = "user";
        turnToken      = turn_cancel_.token();
    }
    compactions_this_turn_ = 0;

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

    for (std::size_t stepNumber = 1;; ++stepNumber) {
        const StepId step = session_.nextStepId();
        session_.append(payload::StepStarted{turn, step});

        drainFoldedItems();
        state_ = AgentState::Thinking;

        if (services_.context == nullptr) {
            appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, "no context assembler");
            return;
        }

        std::vector<Message> messages;
        try {
            messages = services_.context->assemble(session_, TurnContext{turn, step, {}, {}});
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
                runCompaction(messages, turn);
                try {
                    messages =
                        services_.context->assemble(session_, TurnContext{turn, step, {}, {}});
                } catch (const std::exception& error) {
                    appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, error.what());
                    return;
                }
            }
        }

        LLMRequest request = buildRequest(messages);

        const MessageId messageId = make_event_id().value;
        ChunkCoalescer  coalescer(session_, messageId, config_.max_chunk_batch,
                                  config_.chunk_flush_interval);
        std::string          text;
        std::string          reasoning;
        std::optional<Usage> streamUsage;

        StreamSink sink = [&](const StreamEvent& event) -> SinkFlow {
            if (const auto* delta = std::get_if<TextDelta>(&event)) {
                text += delta->text;
                coalescer.onText(delta->text);
            } else if (const auto* delta = std::get_if<ReasoningDelta>(&event)) {
                reasoning += delta->text;
                coalescer.onReasoning(delta->text);
            } else if (const auto* usage = std::get_if<UsageEvent>(&event)) {
                streamUsage = usage->usage;
            }
            return SinkFlow::Continue;
        };

        LLMResponse response;
        bool        slotCancelled = false;
        if (services_.provider == nullptr) {
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
            try {
                response = services_.provider->stream(request, sink, turnToken).get();
            } catch (const std::exception& error) {
                response              = LLMResponse{};
                response.outcome      = StreamOutcome::Failed;
                response.error.code   = LLMErrorCode::ProviderInternal;
                response.error.detail = error.what();
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
            runCompaction(messages, turn);
            try {
                messages = services_.context->assemble(session_, TurnContext{turn, step, {}, {}});
            } catch (const std::exception& error) {
                appendTurnFailed(turn, AgentErrorCode::ContextAssemblyFailed, error.what());
                return;
            }
            request = buildRequest(messages);
        }
        if (slotCancelled) {
            response         = LLMResponse{};
            response.outcome = StreamOutcome::Cancelled;
            response.finish  = FinishReason::Other;
        }

        try {
            coalescer.flush();
        } catch (const LeaseLost& error) {
            appendTurnFailed(turn, AgentErrorCode::LeaseLost, error.what());
            return;
        } catch (const StoreError& error) {
            appendTurnFailed(turn, AgentErrorCode::StoreUnavailable, error.what());
            return;
        }

        std::vector<ContentBlock> content;
        if (!reasoning.empty()) {
            content.push_back(text_block(ContentBlockKind::Reasoning, reasoning));
        }
        if (!text.empty()) {
            content.push_back(text_block(ContentBlockKind::Text, text));
        }
        for (const ToolCallAssembled& call : response.tool_calls) {
            content.push_back(tool_use_block(call));
        }

        payload::AssistantMessage assistant;
        assistant.id      = messageId;
        assistant.content = std::move(content);
        const std::optional<Usage> usage =
            response.usage.has_value() ? response.usage : streamUsage;
        assistant.usage = usage;
        session_.append(assistant);

        std::string cancelReason;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            cancelReason = cancel_reason_.empty() ? std::string{"user"} : cancel_reason_;
        }

        if (response.outcome == StreamOutcome::Cancelled) {
            session_.append(payload::TurnCancelled{turn, cancelReason});
            state_ = AgentState::Idle;
            return;
        }
        if (response.outcome == StreamOutcome::Failed) {
            const std::string detail = response.error.detail.empty()
                                           ? response.error.provider_message
                                           : response.error.detail;
            appendTurnFailed(turn, mapAgentError(response.error.code), detail);
            return;
        }

        if (response.tool_calls.empty()) {
            if (usage.has_value()) {
                session_.append(payload::TokenUsage{*usage, turn});
            }
            session_.append(payload::StepEnded{turn, step});
            session_.append(payload::TurnEnded{turn});
            state_ = AgentState::Idle;
            return;
        }

        state_ = AgentState::CallingTool;
        for (const ToolCallAssembled& call : response.tool_calls) {
            if (turnToken.cancelled()) {
                break;
            }
            executeToolCall(call, turn, step);
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
        if (stepNumber >= config_.max_steps) {
            appendTurnFailed(turn, AgentErrorCode::StepLimitExceeded, "step limit exceeded");
            return;
        }
    }
}

} // namespace ymh

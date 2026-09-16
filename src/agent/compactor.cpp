#include "ymh/agent/compactor.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ymh {
namespace {

constexpr std::string_view kSummaryInstruction =
    "You compress an agent's working history. Preserve, in priority order: "
    "(1) the user's goals and explicit constraints; (2) decisions made and "
    "their rationale; (3) file paths, commands, and tool outcomes that are "
    "still relevant; (4) open tasks and unresolved errors. Omit chit-chat. "
    "Output only the summary. Do not call tools. Do not ask questions.";

Message text_message(Role role, std::string text) {
    Message message;
    message.role = role;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

Sequence effective_boundary(const EventRange& events) {
    Sequence boundary = 0;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::ContextCompaction) {
            boundary = std::max(
                boundary, record.event.payload.get<payload::ContextCompaction>().boundary);
        }
    }
    return boundary;
}

bool is_turn_terminal(EventType type) {
    return type == EventType::TurnEnded || type == EventType::TurnCancelled ||
           type == EventType::TurnFailed;
}

// Largest prefix length <= limit that ends on a UTF-8 code point boundary.
std::size_t utf8_prefix(const std::string& text, std::size_t limit) {
    if (limit >= text.size()) {
        return text.size();
    }
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return cut;
}

std::string truncate_at_block(const std::string& text, std::size_t limit) {
    const std::size_t cut = utf8_prefix(text, limit);
    if (cut == 0) {
        return {};
    }
    const std::size_t newline = text.rfind('\n', cut - 1);
    if (newline != std::string::npos && newline + 1 >= cut / 2) {
        return text.substr(0, newline + 1);
    }
    return text.substr(0, cut);
}

std::string serialize_prefix(const std::vector<Message>& prefix) {
    std::string body;
    for (const Message& message : prefix) {
        body += role_name(message.role);
        body += ": ";
        for (const ContentBlock& block : message.content) {
            switch (block.kind) {
                case ContentBlockKind::Text:
                    body += block.text;
                    break;
                case ContentBlockKind::Reasoning:
                    body += "[reasoning] ";
                    body += block.text;
                    break;
                case ContentBlockKind::ToolUse:
                    body += "[tool_use ";
                    body += block.tool_name;
                    body += " ";
                    body += block.arguments.dump();
                    body += "]";
                    break;
                case ContentBlockKind::Image:
                    body += "[image]";
                    break;
            }
            body += "\n";
        }
    }
    return body;
}

std::string resolve_summarizer_model(const CompactionPolicy& policy, const Session& session) {
    ModelResolutionSources sources;
    if (!policy.summarizer_model.empty()) {
        sources.request_override = policy.summarizer_model;
    }
    if (!session.header().model.empty()) {
        sources.session_model = session.header().model;
    }
    const std::optional<ModelId> resolved = resolve_model(sources);
    return resolved.value_or(session.header().model);
}

} // namespace

ContextCompactor::ContextCompactor(LLMProvider&          provider,
                                   LLMPool&              pool,
                                   const TokenEstimator& estimator,
                                   CompactionPolicy      policy,
                                   WallClock             clock)
    : provider_(provider),
      pool_(pool),
      estimator_(estimator),
      policy_(std::move(policy)),
      clock_(std::move(clock)) {}

ContextCompactor::PlanResult ContextCompactor::select_plan(const Session& session,
                                                           std::size_t keep) const {
    PlanResult result;
    const EventRange events = session.events();

    std::vector<Sequence> points;
    for (const EventRecord& record : events) {
        if (is_turn_terminal(record.event.type)) {
            points.push_back(record.seq);
        }
    }
    if (points.size() <= keep) {
        result.reason = CompactionError::Code::NoBoundary;
        return result;
    }
    const Sequence candidate = points[points.size() - 1 - keep];
    if (candidate <= effective_boundary(events)) {
        result.reason = CompactionError::Code::NoBoundary;
        return result;
    }

    EventRange prefix_events;
    for (const EventRecord& record : events) {
        if (record.seq > candidate) {
            break;
        }
        prefix_events.push_back(record);
    }
    const std::vector<Message> prefix = deriveMessages(session.header(), prefix_events);
    if (prefix.size() < policy_.min_prefix_messages) {
        result.reason = CompactionError::Code::PrefixTooSmall;
        return result;
    }

    const std::vector<Message> full = deriveMessages(session.header(), events);
    result.plan.boundary        = candidate;
    result.plan.prefix_messages = prefix.size();
    result.plan.kept_messages   = full.size() - prefix.size();
    result.plan.valid           = true;
    return result;
}

CompactionPlan ContextCompactor::plan(const Session& session,
                                      const std::vector<Message>& messages) const {
    (void)messages;
    if (!policy_.is_enabled()) {
        return {};
    }
    return select_plan(session, policy_.keep_recent_turns).plan;
}

std::vector<Message> ContextCompactor::build_summary_prompt(
    const std::vector<Message>& prefix) const {
    std::vector<Message> prompt;
    prompt.push_back(text_message(Role::System, std::string{kSummaryInstruction}));
    std::string body = serialize_prefix(prefix);
    if (body.size() > policy_.max_summary_bytes) {
        body = truncate_at_block(body, policy_.max_summary_bytes);
    }
    prompt.push_back(text_message(Role::User, std::move(body)));
    return prompt;
}

std::string ContextCompactor::bound_summary(const std::string& summary) const {
    constexpr std::size_t kPerMessageOverhead = 4;
    constexpr std::size_t kBytesPerToken = 4;
    if (policy_.max_summary_tokens <= kPerMessageOverhead) {
        return {};
    }
    const std::size_t limit = (policy_.max_summary_tokens - kPerMessageOverhead) * kBytesPerToken;
    if (summary.size() <= limit) {
        return summary;
    }
    return truncate_at_block(summary, limit);
}

CompactionResult ContextCompactor::compact(const Session& session,
                                           const std::vector<Message>& messages,
                                           CancellationToken cancel) {
    CompactionResult result;
    if (!policy_.is_enabled()) {
        result.outcome    = CompactionOutcome::NotNeeded;
        result.error.code = CompactionError::Code::Disabled;
        return result;
    }

    PlanResult selected = select_plan(session, policy_.keep_recent_turns);
    if (!selected.plan.valid) {
        result.outcome    = CompactionOutcome::NotNeeded;
        result.error.code = selected.reason;
        return result;
    }

    CompactionPlan current = selected.plan;
    for (int attempt = 0; attempt < 2; ++attempt) {
        EventRange prefix_events;
        for (const EventRecord& record : session.events()) {
            if (record.seq > current.boundary) {
                break;
            }
            prefix_events.push_back(record);
        }
        const std::vector<Message> prefix = deriveMessages(session.header(), prefix_events);
        const std::string          model  = resolve_summarizer_model(policy_, session);

        LLMRequest request;
        request.model                 = model;
        request.messages              = build_summary_prompt(prefix);
        request.parameters.tool_choice = std::string{"none"};

        std::optional<LLMPool::Slot> slot = pool_.acquire(cancel).get();
        if (!slot.has_value()) {
            result.outcome    = CompactionOutcome::Cancelled;
            result.error.code = CompactionError::Code::Cancelled;
            return result;
        }

        std::string summary_text;
        StreamSink  collect = [&](const StreamEvent& event) -> SinkFlow {
            if (const auto* delta = std::get_if<TextDelta>(&event)) {
                summary_text += delta->text;
            }
            return SinkFlow::Continue;
        };
        const LLMResponse response = provider_.stream(request, collect, cancel).get();

        if (response.outcome == StreamOutcome::Cancelled) {
            result.outcome    = CompactionOutcome::Cancelled;
            result.error.code = CompactionError::Code::Cancelled;
            return result;
        }
        if (response.outcome == StreamOutcome::Failed) {
            if (response.error.code == LLMErrorCode::ContextLengthExceeded) {
                if (attempt == 0) {
                    PlanResult reduced = select_plan(session, policy_.keep_recent_turns + 1);
                    if (reduced.plan.valid) {
                        current = reduced.plan;
                        continue;
                    }
                }
                result.outcome    = CompactionOutcome::Failed;
                result.error.code = CompactionError::Code::SummarizerOverflow;
                return result;
            }
            result.outcome    = CompactionOutcome::Failed;
            result.error.code = CompactionError::Code::SummarizerFailed;
            result.error.detail = response.error.detail;
            return result;
        }

        const std::string summary = bound_summary(summary_text);
        if (summary.size() > policy_.max_summary_bytes) {
            result.outcome    = CompactionOutcome::Failed;
            result.error.code = CompactionError::Code::OversizedSummary;
            return result;
        }

        const std::size_t projected = current.prefix_messages + current.kept_messages;
        const std::size_t lead =
            messages.size() >= projected ? messages.size() - projected : 0;
        std::vector<Message> compacted;
        compacted.push_back(text_message(Role::System, summary));
        for (std::size_t index = lead + current.prefix_messages; index < messages.size(); ++index) {
            compacted.push_back(messages[index]);
        }

        payload::ContextCompaction payload;
        payload.boundary      = current.boundary;
        payload.summary       = summary;
        payload.tokenEstimate = estimator_.estimate(compacted);
        payload.model         = model;
        payload.createdAt     = clock_();

        result.outcome    = CompactionOutcome::Compacted;
        result.compaction = std::move(payload);
        result.usage      = response.usage;
        return result;
    }

    result.outcome    = CompactionOutcome::Failed;
    result.error.code = CompactionError::Code::SummarizerOverflow;
    return result;
}

std::optional<payload::ContextCompaction> ContextCompactor::run(
    const Session& session, const std::vector<Message>& messages, CancellationToken cancel) {
    CompactionResult result = compact(session, messages, cancel);
    if (result.outcome == CompactionOutcome::Compacted) {
        return result.compaction;
    }
    return std::nullopt;
}

} // namespace ymh

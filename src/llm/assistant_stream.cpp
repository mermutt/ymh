#include "ymh/llm/assistant_stream.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "ymh/session/errors.hpp"

namespace ymh {
namespace {

ContentBlock text_block(ContentBlockKind kind, const std::string& text) {
    ContentBlock block;
    block.kind = kind;
    block.text = text;
    return block;
}

std::int64_t run_end_ms(std::int64_t time0_ms, const std::vector<std::int64_t>& dt_ms) {
    std::int64_t at = time0_ms;
    for (const std::int64_t dt : dt_ms) {
        at += dt;
    }
    return at;
}

template <class Run>
std::size_t count_runs(const std::vector<AssistantStreamRecord>& records) {
    std::size_t count = 0;
    for (const AssistantStreamRecord& record : records) {
        if (std::holds_alternative<Run>(record)) {
            ++count;
        }
    }
    return count;
}

} // namespace

BlockAssembler::CallState& BlockAssembler::call_for(std::uint32_t index) {
    for (CallState& call : calls_) {
        if (call.index == index) {
            return call;
        }
    }
    calls_.push_back(CallState{index, ToolCallId{}, std::string{}, std::nullopt});
    return calls_.back();
}

void BlockAssembler::push(const StreamEvent& event) {
    if (const auto* delta = std::get_if<TextDelta>(&event)) {
        text_ += delta->text;
    } else if (const auto* delta = std::get_if<ReasoningDelta>(&event)) {
        reasoning_ += delta->text;
    } else if (const auto* started = std::get_if<ToolCallStarted>(&event)) {
        bool exists = false;
        for (const CallState& call : calls_) {
            if (call.index == started->index) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            calls_.push_back(
                CallState{started->index, started->id, started->name, std::nullopt});
        }
    } else if (const auto* finished = std::get_if<ToolCallFinished>(&event)) {
        CallState& call = call_for(finished->index);
        call.id         = finished->call.id;
        call.name       = finished->call.name;
        call.finished   = finished->call;
    } else if (const auto* usage = std::get_if<UsageEvent>(&event)) {
        advisory_usage_ = usage->usage;
    } else if (const auto* finished = std::get_if<Finished>(&event)) {
        finish_ = finished->reason;
        if (finished->usage.has_value()) {
            finished_usage_ = finished->usage;
        }
        replay_state_ = finished->replay_state;
        saw_finished_ = true;
    }
}

std::vector<ContentBlock> BlockAssembler::blocks() const {
    std::vector<ContentBlock> content;
    if (!reasoning_.empty()) {
        content.push_back(text_block(ContentBlockKind::Reasoning, reasoning_));
    }
    if (!text_.empty()) {
        content.push_back(text_block(ContentBlockKind::Text, text_));
    }

    std::vector<const CallState*> finished_calls;
    for (const CallState& call : calls_) {
        if (call.finished.has_value()) {
            finished_calls.push_back(&call);
        }
    }
    std::sort(finished_calls.begin(),
              finished_calls.end(),
              [](const CallState* left, const CallState* right) {
                  return left->index < right->index;
              });
    for (const CallState* call : finished_calls) {
        ContentBlock block;
        block.kind         = ContentBlockKind::ToolUse;
        block.tool_call_id = call->finished->id;
        block.tool_name    = call->finished->name;
        block.arguments    = call->finished->arguments;
        content.push_back(std::move(block));
    }
    return content;
}

std::vector<ContentBlock> BlockAssembler::interrupted_blocks() const {
    std::vector<ContentBlock> content;
    if (!reasoning_.empty()) {
        content.push_back(text_block(ContentBlockKind::Reasoning, reasoning_));
    }
    if (!text_.empty()) {
        content.push_back(text_block(ContentBlockKind::Text, text_));
    }
    return content;
}

std::optional<Usage> BlockAssembler::usage() const {
    if (finished_usage_.has_value()) {
        return finished_usage_;
    }
    return advisory_usage_;
}

FinishReason BlockAssembler::finish() const noexcept {
    return saw_finished_ ? finish_ : FinishReason::Stop;
}

std::optional<ReplayEnvelope> BlockAssembler::replay_state() const {
    return replay_state_;
}

TimedStreamEvent AssistantStreamAccumulator::push(const TimedStreamEvent& timed) {
    const std::int64_t at = timed.at.count();

    if (const auto* delta = std::get_if<TextDelta>(&timed.event)) {
        if (!records_.empty() && std::holds_alternative<TextRun>(records_.back())) {
            TextRun& run = std::get<TextRun>(records_.back());
            run.dt_ms.push_back(at - run_end_ms(run.time0_ms, run.dt_ms));
            run.texts.push_back(delta->text);
        } else {
            TextRun run;
            run.index    = count_runs<TextRun>(records_);
            run.time0_ms = at;
            run.texts.push_back(delta->text);
            records_.push_back(std::move(run));
        }
    } else if (const auto* delta = std::get_if<ReasoningDelta>(&timed.event)) {
        if (!records_.empty() && std::holds_alternative<ReasoningRun>(records_.back())) {
            ReasoningRun& run = std::get<ReasoningRun>(records_.back());
            run.dt_ms.push_back(at - run_end_ms(run.time0_ms, run.dt_ms));
            run.texts.push_back(delta->text);
        } else {
            ReasoningRun run;
            run.index    = count_runs<ReasoningRun>(records_);
            run.time0_ms = at;
            run.texts.push_back(delta->text);
            records_.push_back(std::move(run));
        }
    } else if (const auto* started = std::get_if<ToolCallStarted>(&timed.event)) {
        ToolCallRun run;
        run.index    = started->index;
        run.time0_ms = at;
        run.id       = started->id;
        run.name     = started->name;
        records_.push_back(std::move(run));
    } else if (const auto* delta = std::get_if<ToolCallDelta>(&timed.event)) {
        bool appended = false;
        if (!records_.empty() && std::holds_alternative<ToolCallRun>(records_.back())) {
            ToolCallRun& run = std::get<ToolCallRun>(records_.back());
            if (run.index == delta->index) {
                run.dt_ms.push_back(at - run_end_ms(run.time0_ms, run.dt_ms));
                run.args.push_back(delta->arguments_fragment);
                appended = true;
            }
        }
        if (!appended) {
            records_.push_back(ChunkRecord{at, timed.event});
        }
    } else {
        records_.push_back(ChunkRecord{at, timed.event});
    }

    return timed;
}

std::vector<AssistantStreamRecord> AssistantStreamAccumulator::snapshot() const {
    return records_;
}

std::vector<TimedStreamEvent> expand(const std::vector<AssistantStreamRecord>& snapshot) {
    std::vector<TimedStreamEvent> out;

    const auto push_text_like = [&out](const std::string& kind,
                                       std::int64_t time0_ms,
                                       const std::vector<std::int64_t>& dt_ms,
                                       const std::vector<std::string>& texts,
                                       bool reasoning) {
        if (texts.empty()) {
            throw CorruptionError("assistant stream: empty " + kind + " run");
        }
        if (time0_ms < 0) {
            throw CorruptionError("assistant stream: negative " + kind + ".time0_ms");
        }
        if (dt_ms.size() + 1 != texts.size()) {
            throw CorruptionError("assistant stream: " + kind + " array-size invariant violated");
        }
        std::int64_t at = time0_ms;
        for (std::size_t index = 0; index < texts.size(); ++index) {
            if (index > 0) {
                if (dt_ms[index - 1] < 0) {
                    throw CorruptionError("assistant stream: negative dt_ms");
                }
                at += dt_ms[index - 1];
            }
            if (reasoning) {
                out.push_back(TimedStreamEvent{std::chrono::milliseconds{at},
                                               ReasoningDelta{texts[index]}});
            } else {
                out.push_back(TimedStreamEvent{std::chrono::milliseconds{at},
                                               TextDelta{texts[index]}});
            }
        }
    };

    for (const AssistantStreamRecord& record : snapshot) {
        if (const auto* run = std::get_if<TextRun>(&record)) {
            push_text_like("TextRun", run->time0_ms, run->dt_ms, run->texts, false);
        } else if (const auto* run = std::get_if<ReasoningRun>(&record)) {
            push_text_like("ReasoningRun", run->time0_ms, run->dt_ms, run->texts, true);
        } else if (const auto* run = std::get_if<ToolCallRun>(&record)) {
            if (run->id.empty()) {
                throw CorruptionError("assistant stream: empty ToolCallRun.id");
            }
            if (run->time0_ms < 0) {
                throw CorruptionError("assistant stream: negative ToolCallRun.time0_ms");
            }
            if (run->dt_ms.size() != run->args.size()) {
                throw CorruptionError(
                    "assistant stream: ToolCallRun array-size invariant violated");
            }
            const auto index = static_cast<std::uint32_t>(run->index);
            out.push_back(TimedStreamEvent{
                std::chrono::milliseconds{run->time0_ms},
                ToolCallStarted{index, run->id, run->name.value_or(std::string{})}});
            std::int64_t at = run->time0_ms;
            for (std::size_t delta = 0; delta < run->args.size(); ++delta) {
                if (run->dt_ms[delta] < 0) {
                    throw CorruptionError("assistant stream: negative dt_ms");
                }
                at += run->dt_ms[delta];
                out.push_back(TimedStreamEvent{
                    std::chrono::milliseconds{at},
                    ToolCallDelta{index, run->args[delta]}});
            }
        } else if (const auto* chunk = std::get_if<ChunkRecord>(&record)) {
            out.push_back(TimedStreamEvent{std::chrono::milliseconds{chunk->time_ms},
                                           chunk->event});
        }
    }
    return out;
}

} // namespace ymh

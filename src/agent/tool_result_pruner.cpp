#include "ymh/agent/tool_result_pruner.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/session/events.hpp"

namespace ymh {
namespace {

bool is_continuation(unsigned char byte) noexcept {
    return (byte & 0xC0u) == 0x80u;
}

std::size_t code_point_length(std::string_view text) noexcept {
    std::size_t count = 0;
    for (const char ch : text) {
        if (!is_continuation(static_cast<unsigned char>(ch))) {
            ++count;
        }
    }
    return count;
}

std::size_t byte_offset_after_code_points(std::string_view text, std::size_t count) {
    std::size_t seen = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (is_continuation(static_cast<unsigned char>(text[index]))) {
            continue;
        }
        if (seen == count) {
            return index;
        }
        ++seen;
    }
    return text.size();
}

std::size_t byte_offset_before_last_code_points(std::string_view text, std::size_t count) {
    if (count == 0) {
        return text.size();
    }
    std::size_t seen = 0;
    for (std::size_t index = text.size(); index > 0; --index) {
        if (is_continuation(static_cast<unsigned char>(text[index - 1]))) {
            continue;
        }
        ++seen;
        if (seen == count) {
            return index - 1;
        }
    }
    return 0;
}

std::string code_point_head(std::string_view text, std::size_t count) {
    return std::string{text.substr(0, byte_offset_after_code_points(text, count))};
}

std::string code_point_tail(std::string_view text, std::size_t count) {
    return std::string{text.substr(byte_offset_before_last_code_points(text, count))};
}

std::uint64_t shadowed_token_count(const std::string& output) {
    Message message;
    message.role = Role::Tool;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = output;
    message.content.push_back(std::move(block));
    return static_cast<std::uint64_t>(DefaultTokenEstimator{}.estimate({message}));
}

} // namespace

PruneResult ToolResultPruner::prune_session(Session& session) {
    const EventRange events = session.events();
    std::map<ToolCallId, std::pair<Sequence, payload::ToolResult>> surface;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::ToolResult) {
            continue;
        }
        const auto& result = record.event.payload.get<payload::ToolResult>();
        surface[result.id] = {record.seq, result};
    }

    PruneResult pruned;
    for (const auto& [id, entry] : surface) {
        const Sequence             seq      = entry.first;
        const payload::ToolResult& original = entry.second;
        if (code_point_length(original.output) <= config_.threshold_code_points) {
            continue;
        }
        const std::string_view output = original.output;
        payload::ToolResult    replacement = original;
        replacement.output = code_point_head(output, config_.head_code_points) +
                             std::string{kPruneMarker} +
                             code_point_tail(output, config_.tail_code_points);

        payload::ContextPrune prune;
        prune.shadowedStart      = seq;
        prune.shadowedEnd        = seq;
        prune.shadowedSeqs       = {seq};
        prune.shadowedTokenCount = shadowed_token_count(original.output);

        session.append(prune);
        const Sequence replacement_seq = session.append(replacement);
        pruned.replacements.push_back(replacement_seq);
        ++pruned.pruned;
    }
    return pruned;
}

} // namespace ymh

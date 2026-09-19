#pragma once

// The canonical assistant stream: the block-assembly algorithm and the timed
// delta accumulator, pinned by 34-assembler-replay-errata.md §3-§8 (26 §4.3.4
// shapes). Included only by the agent loop and tests (34-D1); no adapter
// includes it.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/message.hpp"
#include "ymh/llm/stream.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {

// 26 §4.3.4 :559-566 (34 §3.2).
struct TextRun {
    std::size_t                index;
    std::int64_t               time0_ms;
    std::vector<std::int64_t>  dt_ms;
    std::vector<std::string>   texts;

    bool operator==(const TextRun&) const = default;
};

struct ReasoningRun {
    std::size_t                index;
    std::int64_t               time0_ms;
    std::vector<std::int64_t>  dt_ms;
    std::vector<std::string>   texts;

    bool operator==(const ReasoningRun&) const = default;
};

struct ToolCallRun {
    std::size_t                    index;
    std::int64_t                   time0_ms;
    std::vector<std::int64_t>      dt_ms;
    ToolCallId                     id;
    std::optional<std::string>     name;
    std::vector<std::string>       args;

    bool operator==(const ToolCallRun&) const = default;
};

struct ChunkRecord {
    std::int64_t  time_ms;
    StreamEvent   event;

    bool operator==(const ChunkRecord&) const = default;
};

using AssistantStreamRecord = std::variant<TextRun, ReasoningRun, ToolCallRun, ChunkRecord>;

// 34 §4 (34-D2): the canonical chunk->message algorithm. Default-constructible,
// no clock, no error surface, no thread affinity beyond its owning attempt.
class BlockAssembler {
public:
    void push(const StreamEvent& event);

    [[nodiscard]] std::vector<ContentBlock> blocks() const;
    [[nodiscard]] std::vector<ContentBlock> interrupted_blocks() const;
    [[nodiscard]] std::optional<Usage>      usage() const;
    [[nodiscard]] FinishReason              finish() const noexcept;
    [[nodiscard]] std::optional<ReplayEnvelope> replay_state() const;

private:
    struct CallState {
        std::uint32_t                    index = 0;
        ToolCallId                       id;
        std::string                      name;
        std::optional<ToolCallAssembled> finished;
    };

    [[nodiscard]] CallState& call_for(std::uint32_t index);

    std::string                        text_;
    std::string                        reasoning_;
    std::vector<CallState>             calls_;
    std::optional<Usage>               finished_usage_;
    std::optional<Usage>               advisory_usage_;
    FinishReason                       finish_ = FinishReason::Stop;
    bool                               saw_finished_ = false;
    std::optional<ReplayEnvelope>      replay_state_;
};

// 34 §8 (34-D7): packs a per-attempt timed stream into maximal same-kind runs.
// `push` returns a detached copy of the recorded event (26 §4.3.4 :571-575).
class AssistantStreamAccumulator {
public:
    TimedStreamEvent push(const TimedStreamEvent& timed);

    [[nodiscard]] std::vector<AssistantStreamRecord> snapshot() const;

private:
    std::vector<AssistantStreamRecord> records_;
};

// 34 §8.5 (34-D8): reconstructs the exact pushed `TimedStreamEvent` sequence.
// Throws `CorruptionError` on a semantically invalid record.
[[nodiscard]] std::vector<TimedStreamEvent> expand(
    const std::vector<AssistantStreamRecord>& snapshot);

// ---------------------------------------------------------------------------
// JSON codec (33 §3.3/§4). Lives with the records (33-D5) so `events.cpp` can
// embed it without a second definition.
// ---------------------------------------------------------------------------

inline void to_json(nlohmann::json& json, const TextRun& run) {
    json = nlohmann::json{
        {"type", "text_chunks"},
        {"index", run.index},
        {"time0_ms", run.time0_ms},
        {"dt_ms", run.dt_ms},
        {"texts", run.texts},
    };
}

inline void from_json(const nlohmann::json& json, TextRun& run) {
    run.index    = json.value("index", std::size_t{0});
    run.time0_ms = json.value("time0_ms", std::int64_t{0});
    run.dt_ms    = json.value("dt_ms", std::vector<std::int64_t>{});
    run.texts    = json.value("texts", std::vector<std::string>{});
}

inline void to_json(nlohmann::json& json, const ReasoningRun& run) {
    json = nlohmann::json{
        {"type", "reasoning_chunks"},
        {"index", run.index},
        {"time0_ms", run.time0_ms},
        {"dt_ms", run.dt_ms},
        {"texts", run.texts},
    };
}

inline void from_json(const nlohmann::json& json, ReasoningRun& run) {
    run.index    = json.value("index", std::size_t{0});
    run.time0_ms = json.value("time0_ms", std::int64_t{0});
    run.dt_ms    = json.value("dt_ms", std::vector<std::int64_t>{});
    run.texts    = json.value("texts", std::vector<std::string>{});
}

inline void to_json(nlohmann::json& json, const ToolCallRun& run) {
    json = nlohmann::json{
        {"type", "tool_call_chunks"},
        {"index", run.index},
        {"time0_ms", run.time0_ms},
        {"dt_ms", run.dt_ms},
        {"id", run.id},
        {"args", run.args},
    };
    if (run.name.has_value()) {
        json["name"] = *run.name;
    }
}

inline void from_json(const nlohmann::json& json, ToolCallRun& run) {
    run.index    = json.value("index", std::size_t{0});
    run.time0_ms = json.value("time0_ms", std::int64_t{0});
    run.dt_ms    = json.value("dt_ms", std::vector<std::int64_t>{});
    run.id       = json.value("id", ToolCallId{});
    run.args     = json.value("args", std::vector<std::string>{});
    if (json.contains("name") && !json.at("name").is_null()) {
        run.name = json.at("name").get<std::string>();
    } else {
        run.name = std::nullopt;
    }
}

inline void to_json(nlohmann::json& json, const ChunkRecord& record) {
    json = nlohmann::json{
        {"type", "chunk"},
        {"time_ms", record.time_ms},
        {"event", record.event},
    };
}

inline void from_json(const nlohmann::json& json, ChunkRecord& record) {
    record.time_ms = json.value("time_ms", std::int64_t{0});
    record.event   = json.at("event").get<StreamEvent>();
}

inline void to_json(nlohmann::json& json, const AssistantStreamRecord& record) {
    std::visit([&json](const auto& value) { json = value; }, record);
}

inline void from_json(const nlohmann::json& json, AssistantStreamRecord& record) {
    const std::string type = json.at("type").get<std::string>();
    if (type == "text_chunks") {
        record = json.get<TextRun>();
    } else if (type == "reasoning_chunks") {
        record = json.get<ReasoningRun>();
    } else if (type == "tool_call_chunks") {
        record = json.get<ToolCallRun>();
    } else if (type == "chunk") {
        record = json.get<ChunkRecord>();
    } else {
        throw nlohmann::json::other_error::create(
            501, "unknown AssistantStreamRecord type: " + type, &json);
    }
}

} // namespace ymh

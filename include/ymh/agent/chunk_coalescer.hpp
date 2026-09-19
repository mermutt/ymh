#pragma once

// Delta -> live-only `AssistantChunk` publication, owned by the agent loop
// (06-agent-loop.md §5.4; 29-D4). One instance per provider attempt (34-D5).
// Bounds come from 02 §6.2: flush when the pending batch reaches
// `max_chunk_batch`, when `chunk_flush_interval` elapses, or at a barrier event
// (A4/A5). A flush is a sequence of live-only `Session::emit` publications
// (never a durable append) and preserves delta order.

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/session/events.hpp"
#include "ymh/session/ids.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

class ChunkCoalescer {
public:
    ChunkCoalescer(Session& session,
                   MessageId message,
                   std::size_t maxBatch,
                   std::chrono::milliseconds flushInterval);

    void onText(std::string_view text);
    void onReasoning(std::string_view text);

    // Barrier: flush before any non-chunk append (A5).
    void flush();

    [[nodiscard]] bool empty() const noexcept { return pending_.empty(); }
    [[nodiscard]] std::size_t pendingCount() const noexcept { return pending_.size(); }

private:
    void push(payload::AssistantChunkKind kind, std::string_view text);
    void maybeFlush();

    Session&                  session_;
    MessageId                 message_;
    std::size_t               maxBatch_;
    std::chrono::milliseconds flushInterval_;
    std::size_t               nextIndex_ = 0;
    std::chrono::steady_clock::time_point lastFlush_;
    std::vector<payload::AssistantChunk>  pending_;
};

} // namespace ymh

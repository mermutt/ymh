#include "ymh/agent/chunk_coalescer.hpp"

#include <chrono>
#include <utility>

#include "ymh/core/event.hpp"

namespace ymh {

ChunkCoalescer::ChunkCoalescer(Session& session,
                               MessageId message,
                               std::size_t maxBatch,
                               std::chrono::milliseconds flushInterval)
    : session_(session),
      message_(std::move(message)),
      maxBatch_(maxBatch == 0 ? 1 : maxBatch),
      flushInterval_(flushInterval),
      lastFlush_(std::chrono::steady_clock::now()) {}

void ChunkCoalescer::onText(std::string_view text) {
    push(payload::AssistantChunkKind::Text, text);
}

void ChunkCoalescer::onReasoning(std::string_view text) {
    push(payload::AssistantChunkKind::Reasoning, text);
}

void ChunkCoalescer::push(payload::AssistantChunkKind kind, std::string_view text) {
    payload::AssistantChunk chunk;
    chunk.message = message_;
    chunk.index   = nextIndex_++;
    chunk.text    = std::string{text};
    chunk.kind    = kind;
    pending_.push_back(std::move(chunk));
    if (pending_.size() >= maxBatch_) {
        flush();
    } else {
        maybeFlush();
    }
}

void ChunkCoalescer::maybeFlush() {
    if (pending_.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastFlush_ >= flushInterval_) {
        flush();
    }
}

void ChunkCoalescer::flush() {
    if (pending_.empty()) {
        return;
    }

    for (const payload::AssistantChunk& chunk : pending_) {
        TypedEvent<payload::AssistantChunk> typed;
        typed.id         = make_event_id();
        typed.session_id = session_.id();
        typed.timestamp  = std::chrono::system_clock::now();
        typed.payload    = chunk;
        session_.emit(encode(typed));
    }

    pending_.clear();
    lastFlush_ = std::chrono::steady_clock::now();
}

} // namespace ymh

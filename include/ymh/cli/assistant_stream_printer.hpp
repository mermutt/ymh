#pragma once

// The shared stdout printer for the two `ymh run` paths (the in-process runner
// and the daemon-connected receiver). It owns the per-message live-delta buffer
// so a failed provider attempt's streamed text can be discarded before a retry
// re-streams under the same per-step `messageId` (34 §15 item 1). Printing each
// delta immediately would make that text impossible to remove from a
// forward-only stdout, producing the cross-attempt double-print; this is the
// stream analogue of the TUI's replace-on-finish (`ui_model.cpp`).

#include <cstddef>
#include <iosfwd>
#include <string>

#include "ymh/core/event.hpp"

namespace ymh {

class AssistantStreamPrinter {
public:
    // The result of feeding one event. `handled` is true when the event was an
    // assistant stream/settlement event (AssistantChunk, AssistantMessage,
    // AssistantAttempt); `text` is the assistant text committed to `out`.
    struct Commit {
        bool        handled = false;
        std::string text;
    };

    // F8: the live buffer is bounded. A long (or never-settling) stream must not
    // accumulate the whole response in memory. The durable settlement is printed
    // in full independently of the buffer, so this cap only truncates the
    // live-only fallback. Policy: each buffer (text and reasoning) appends only
    // up to `kMaxBufferedBytes`; further deltas for that buffer are dropped.
    static constexpr std::size_t kMaxBufferedBytes = 1u << 20;   // 1 MiB

    // Consumes one event. Live `AssistantChunk` deltas are buffered per message
    // id; the buffer is committed on an `AssistantMessage` settlement (falling
    // back to the durable assembled content when no live delta arrived, e.g.
    // resume/replay) and discarded on an `AssistantAttempt` (Failed/Cancelled)
    // settlement. When `print_reasoning` is set, buffered reasoning is written to
    // `err` on commit.
    Commit feed(const Event& event, std::ostream& out, std::ostream& err, bool print_reasoning);

private:
    std::string message_;
    std::string text_;
    std::string reasoning_;
};

} // namespace ymh

#pragma once

// The shared stdout printer for the two `ymh run` paths (the in-process runner
// and the daemon-connected receiver). 34 §15 item 1 / decision 34-D13: live
// `Text` deltas are written to `out` as they arrive; a `Completed`
// `AssistantMessage` settlement prints only the not-yet-streamed durable suffix
// (the durable is the authority), deduped by a bounded record plus an O(1)
// streamed-byte counter; a non-`Completed` `AssistantAttempt` cannot retract
// its already-streamed text from a forward-only stdout, so it emits a visible
// retry boundary marker on `err` and discards the record.

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

#include "ymh/core/event.hpp"

namespace ymh {

class AssistantStreamPrinter {
public:
    // The result of feeding one event. `handled` is true when the event was an
    // assistant stream/settlement event (AssistantChunk, AssistantMessage,
    // AssistantAttempt); `text` is the settlement's authoritative assistant
    // text (the durable settlement text, else the live fallback record). It is
    // non-empty only for an `AssistantMessage` and is not the same quantity as
    // the bytes newly written to `out`, because deltas stream directly.
    struct Commit {
        bool        handled = false;
        std::string text;
    };

    // F8: the retained record is bounded. A long (or never-settling) stream must
    // not accumulate the whole response in memory. The durable settlement is
    // printed in full independently of the record, and `streamed_total_` (an
    // O(1) counter) lets the settlement skip the already-streamed prefix without
    // retaining it. Policy: each buffer (text and reasoning) appends only up to
    // `kMaxBufferedBytes`; further deltas for that buffer are dropped from the
    // record (text deltas are still streamed to `out`).
    static constexpr std::size_t kMaxBufferedBytes = 1u << 20;   // 1 MiB

    // 34-I14 / R3: written to `err` on a non-`Completed` `AssistantAttempt`
    // that has already streamed text. The leading newline breaks the streamed
    // line so the boundary is visible on a forward-only stdout.
    static constexpr std::string_view kRetryMarker = "\n[retry]\n";

    // Consumes one event. Live `AssistantChunk` `Text` deltas are written to
    // `out` immediately and recorded (bounded) for settlement dedup; `Reasoning`
    // deltas are buffered only. On `AssistantMessage`, only the not-yet-streamed
    // durable suffix is printed (falling back to the durable in full when
    // nothing was streamed or the live prefix diverged), and the durable is the
    // authority for `Commit::text`. On `AssistantAttempt`, `kRetryMarker` is
    // written to `err` when text was streamed, and the record is discarded.
    // When `print_reasoning` is set, buffered reasoning is written to `err` on
    // commit.
    Commit feed(const Event& event, std::ostream& out, std::ostream& err, bool print_reasoning);

private:
    std::string message_;
    std::string text_;
    std::size_t streamed_total_ = 0;
    std::string reasoning_;
};

} // namespace ymh

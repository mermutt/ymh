#pragma once

// The daemon-backed `ymh run` event-stream receiver. This is the second wire
// receiver beside `ui::SupervisorConnection::dispatch`; both apply the same
// 29-D5 / 29 §3.3 Axis B skip contract: an envelope whose wire event type this
// binary does not know arrives with `event_skipped` set and MUST be dropped
// without dispatch, so the default-constructed sentinel (`EventType{}` ==
// `SessionStarted`, null payload) is never mistaken for a real event.
//
// Extracted from `run_via_daemon` so the contract is unit-testable without a
// live daemon.

#include <iosfwd>

#include "ymh/transport/protocol.hpp"

namespace ymh {

// What the receiver should do after consuming one `event.stream` notification.
enum class StreamDisposition {
    Skipped,       // unknown wire type: drop the envelope, keep streaming
    Continue,      // consumed; keep streaming
    TurnFailed,    // `turn/fail`: report and stop (exit non-zero)
    TurnFinished,  // `turn/end` | `turn/cancel`: report and stop (exit zero)
};

// Handles one `event.stream` notification, writing assistant text / failures to
// `out` / `err` and reporting the loop action. A skipped envelope writes nothing
// and must not be dispatched.
[[nodiscard]] StreamDisposition handle_stream_notification(
    const protocol::StreamNotification& stream, std::ostream& out, std::ostream& err);

} // namespace ymh

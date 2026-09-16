#pragma once

// `Executor` — the daemon-loop marshalling seam (14 §5.1, E-P3).
//
// The daemon has exactly one `asio::io_context` (04 §3.4) and a bounded
// `TurnExecutor` for agent turns (11 §3.2, E6). Tools run on `TurnExecutor`
// workers; the PTY pump must run on the loop so it never blocks a worker and
// never races the transport. This header pins the minimal seam so the execution
// layer does not include Asio headers in its interface.
//
// The SHAPE is pinned here (E-P3): 04 §3.4 names the one-loop model but defines
// no `Executor` type, so this spec owns the seam and 04 owns the concrete
// `AsioExecutor` adapter (`include/ymh/execution/asio_executor.hpp`).

#include <functional>

namespace ymh {

class Executor {
public:
    virtual ~Executor() = default;

    // Enqueue onto the loop. Thread-safe; never starts a thread (11 E1/E2).
    // Returns false (and does NOT invoke) when the loop has stopped.
    virtual bool post(std::function<void()>) = 0;

    // True once the loop's runner has stopped; makes stopped-loop detection
    // explicit instead of inferred (E-P3).
    virtual bool stopped() const noexcept = 0;

    // True iff the calling thread is the loop's runner thread. Used by asserts
    // and TSan tests (11 E2).
    virtual bool onLoopThread() const noexcept = 0;
};

} // namespace ymh

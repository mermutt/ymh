#pragma once

// 46-D8.6 — the one total clamp helper for the per-tool-run deadline.
//
// The shipped primitives already give `0` a meaning of its own:
//   * `ProcessRequest::timeout == 0`  => no deadline (`process.hpp`);
//   * PTY `read(wait == 0)`           => non-blocking poll (`pty.cpp`);
//   * PTY `wait(timeout == 0)`        => block until exit (`pty.cpp`);
//   * MCP `options.timeout == 0`      => fall back to the 60 s server default.
//
// A raw `min(caller, remaining)` is therefore inverted by an expired deadline
// (`min(x, 0) == 0`, which every primitive reads as "no deadline"). This helper
// keeps the three states distinct: `nullopt` disabled, `0ms` expired (an
// immediate timeout, never `nullopt` and never a `0`-as-disabled), `>0` a budget.

#include <chrono>
#include <optional>

namespace ymh {

// The effective primitive timeout for a call.
//   nullopt          -> no deadline (the primitive's own semantics apply)
//   milliseconds{0}  -> the deadline is already expired: time out IMMEDIATELY
//   milliseconds{>0} -> the budget to wait
// Rules: expired => 0 (never nullopt/0-as-disabled); caller == 0 => remaining;
//        else min(caller, remaining).
[[nodiscard]] std::optional<std::chrono::milliseconds>
clamp_timeout(const std::optional<std::chrono::steady_clock::time_point>& deadline,
              std::chrono::milliseconds caller_ms,
              std::chrono::steady_clock::time_point now);

} // namespace ymh

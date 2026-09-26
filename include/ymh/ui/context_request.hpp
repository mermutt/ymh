#pragma once

// 18 §4.3 (M9/C1): the generation guard for `context.show` replies. A reply
// that arrives after `Esc` (or after a newer `r` request) must not re-open or
// overwrite the dismissed overlay. Every request is tagged with a monotonically
// increasing generation; a reply is applied only while its generation still
// matches, and closing/superseding bumps the generation.
//
// The counter is `std::atomic<std::uint64_t>` because it is written on the UI
// thread (open/close) and read on the pump thread (reply early-out). Relaxed
// ordering is sufficient: it guards no other memory, the reply payload is
// captured by value, and every `model_` write happens on the UI thread, so the
// UI-thread check is authoritative. These helpers keep that contract testable.

#include <atomic>
#include <cstdint>

namespace ymh::ui {

[[nodiscard]] inline std::uint64_t bump_context_generation(
    std::atomic<std::uint64_t>& generation) noexcept {
    return generation.fetch_add(1, std::memory_order_relaxed) + 1;
}

[[nodiscard]] inline bool context_reply_is_current(
    const std::atomic<std::uint64_t>& generation, std::uint64_t request) noexcept {
    return request == generation.load(std::memory_order_relaxed);
}

} // namespace ymh::ui

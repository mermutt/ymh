#pragma once

// Incremental Server-Sent Events parser (08-llm-provider.md §6.2). Consumes
// transport bytes as they arrive, so `data:` frames split across TCP chunk
// boundaries reassemble correctly. A single physical line longer than the cap
// aborts with `MalformedResponse` (L-F16); memory stays bounded.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ymh {

struct SseEvent {
    std::string data;  // data lines joined with '\n'
    bool        done = false;  // literal `data: [DONE]`
};

using SseEventSink = std::function<void(const SseEvent&)>;

enum class SseFeedStatus : std::uint8_t {
    Ok,
    Overflow,  // a line exceeded `max_line_bytes`
};

class SseParser {
public:
    explicit SseParser(std::size_t max_line_bytes);

    // Feeds bytes; emits every completed event through `sink`. Returns
    // `Overflow` (and stops) when a single line exceeds the cap.
    SseFeedStatus feed(std::string_view bytes, const SseEventSink& sink);

    // Flushes a trailing event when the stream ended without a blank line.
    void finish(const SseEventSink& sink);

    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] bool done() const noexcept { return done_; }

private:
    void handleLine(std::string_view line, const SseEventSink& sink);
    void dispatch(const SseEventSink& sink);

    std::size_t max_line_bytes_;
    std::string line_;
    std::string data_;
    bool        has_data_ = false;
    bool        overflowed_ = false;
    bool        done_ = false;
};

} // namespace ymh

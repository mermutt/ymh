#pragma once

// Tool output streaming and bounded ring buffers (07 §5.2, F5, §9.11).
//
// `OutputRing` is the live-only bounded buffer (one per session, owned by the
// `ResourceGovernor`); `OutputSink` is the per-call writer a tool streams into.
// The sink is the UTF-8 boundary: raw bytes are lossy-converted to valid UTF-8
// before they enter the ring, because `payload::ToolResult.output` is JSON text
// (01 S10). Neither type ever becomes durable; the loop appends the bounded
// snapshot `materialize()` returns (X8/X10).

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ymh {

// Bounded, tail-keeping, non-blocking. On overflow the oldest bytes are evicted
// and `truncated()` latches true; capacity is never exceeded.
class OutputRing {
public:
    explicit OutputRing(std::size_t capacity_bytes);

    void        append(std::string_view chunk, bool is_stderr);
    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept;
    bool        truncated() const noexcept;

    // Most recent `max_bytes` bytes, trimmed to a valid UTF-8 boundary.
    std::string tail(std::size_t max_bytes) const;

    void clear() noexcept;

private:
    void evict_to_fit();

    std::size_t       capacity_;
    std::deque<char>  bytes_;
    std::size_t       total_appended_ = 0;
    bool              truncated_ = false;
};

// Per-call writer. `write`/`writeErr` may be called many times; `close` is the
// idempotent final flush barrier. All methods are safe to call concurrently with
// each other from the owning call's threads.
class OutputSink {
public:
    virtual ~OutputSink() = default;

    virtual void write(std::string_view chunk) = 0;
    virtual void writeErr(std::string_view chunk) = 0;
    virtual void close() = 0;

    [[nodiscard]] virtual std::size_t bytesWritten() const noexcept = 0;
    [[nodiscard]] virtual bool        truncated() const noexcept = 0;

    [[nodiscard]] virtual std::string materialize(std::size_t max_bytes) const = 0;
};

// The concrete sink used by the loop: sanitizes to UTF-8, appends to a ring,
// and materializes a bounded snapshot.
class RingOutputSink final : public OutputSink {
public:
    explicit RingOutputSink(OutputRing& ring);

    void write(std::string_view chunk) override;
    void writeErr(std::string_view chunk) override;
    void close() override;

    [[nodiscard]] std::size_t bytesWritten() const noexcept override;
    [[nodiscard]] bool        truncated() const noexcept override;
    [[nodiscard]] std::string materialize(std::size_t max_bytes) const override;

private:
    OutputRing* ring_;
    std::size_t bytes_written_ = 0;
    bool        utf8_loss_ = false;
};

// 83 §2.2 (A2): per-call writer. Owns the call's own bounded ring, which is the
// ONLY source `materialize()` reads, so a `ToolResult` can never contain another
// call's, session's, or subagent's bytes. Each sanitized chunk is optionally
// fanned out to the session's live ring (`live`), which stays the UI live buffer
// (07 §8.2) and never feeds `materialize()`. `live` is held by an owning
// `shared_ptr` (83 NEW-1) so the sink keeps the ring alive for the call even if
// the governor's map entry is evicted mid-call. Built on the call's stack; not
// copyable or movable (a copy would share `live_` and split `call_ring_`).
class CallOutputSink final : public OutputSink {
public:
    // `per_call_capacity` is `ResourceCaps::session_output_ring_bytes`; `live`
    // may be null (governor-less loop, tests) => no live tee.
    CallOutputSink(std::size_t per_call_capacity, std::shared_ptr<OutputRing> live);

    CallOutputSink(const CallOutputSink&) = delete;
    CallOutputSink& operator=(const CallOutputSink&) = delete;
    CallOutputSink(CallOutputSink&&) = delete;
    CallOutputSink& operator=(CallOutputSink&&) = delete;

    void write(std::string_view chunk) override;     // sanitize once; append to
    void writeErr(std::string_view chunk) override;  //  call_ring_ + *live_
    void close() override;                           // idempotent no-op barrier

    [[nodiscard]] std::size_t bytesWritten() const noexcept override;
    [[nodiscard]] bool        truncated() const noexcept override;
    [[nodiscard]] std::string materialize(std::size_t max_bytes) const override;

private:
    OutputRing                 call_ring_;  // authoritative result buffer
    std::shared_ptr<OutputRing> live_;      // nullable; keeps the live ring alive
    std::size_t                bytes_written_ = 0;
    bool                       utf8_loss_ = false;
};

// Lossy-converts `bytes` to valid UTF-8 (invalid sequences become U+FFFD) and
// reports whether any byte was replaced. Exposed for the UTF-8 boundary tests
// (E-F10, §5.2/§8.2).
[[nodiscard]] std::string sanitize_utf8(std::string_view bytes, bool& lost);

// Trims leading continuation bytes so a tail slice begins on a codepoint.
[[nodiscard]] std::string_view trim_to_utf8_start(std::string_view bytes) noexcept;

} // namespace ymh

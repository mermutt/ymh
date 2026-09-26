#pragma once

// One shared EventBus with global and SessionId-routed delivery
// (00-architecture.md §8.3, 01-session.md §5).
//
// Contract (I6, I20):
//   * events for session X are delivered in publish order
//   * events for different sessions may interleave arbitrarily
//   * global cross-session ordering is not guaranteed and is not required
//
// There are no N child buses (D12). Per-session ordering is owned by that
// session's SessionMailbox, which sits between the bus and each consumer.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "ymh/core/event.hpp"

namespace ymh {

// Ordered FIFO for one session (01 §5). Owned by the bus, one per session.
// The mailbox is the ordering authority for a session; teardown may drain it
// before a closed session's state is freed (F3, §8.3).
class SessionMailbox {
public:
    SessionMailbox() = default;

    SessionMailbox(const SessionMailbox&) = delete;
    SessionMailbox& operator=(const SessionMailbox&) = delete;
    SessionMailbox(SessionMailbox&&) = delete;
    SessionMailbox& operator=(SessionMailbox&&) = delete;

    ~SessionMailbox() = default;

    // Preserves publish order.
    void push(Event event);

    // Pops events in FIFO order and passes each to `sink`. `sink` returns
    // false to stop draining. Returns true when the mailbox is empty.
    bool drain(const std::function<bool(Event&&)>& sink);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    mutable std::mutex mutex_;
    std::deque<Event>  queue_;
};

namespace detail {

// Shared bus state. Kept opaque so Subscription can hold a weak reference
// without exposing the bus internals.
struct EventBusState;

// Wraps a payload handler into an erased `Event` handler. `Payload == Event`
// is the already-erased case; anything else is decoded from JSON (S3).
template <class Payload>
[[nodiscard]] std::function<void(const Event&)> erase_handler(
    std::function<void(const Payload&)> handler) {
    if constexpr (std::is_same_v<Payload, Event>) {
        return handler;
    } else {
        return [handler = std::move(handler)](const Event& event) {
            handler(decode<Payload>(event).payload);
        };
    }
}

} // namespace detail

// RAII subscription handle; unsubscribes on destruction (00 §8.3).
//
// 24-D12 (spec 24 §6): the handle carries the channel it was created on so a
// committed handler stored in its own list is erased by the matching
// `unsubscribe` (AL29; otherwise `~HostRuntime` would leak it and a later
// publish would call into a destroyed `HostRuntime`, AL-F20).
class Subscription {
public:
    enum class Channel : std::uint8_t { Global, Session, Committed };

    Subscription() noexcept = default;
    ~Subscription();

    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    // Idempotent.
    void unsubscribe() noexcept;

    [[nodiscard]] bool active() const noexcept;

private:
    friend class EventBus;

    Subscription(
        std::weak_ptr<detail::EventBusState> state,
        std::uint64_t                        id,
        std::optional<std::string>           session,
        Channel                              channel) noexcept;

    std::weak_ptr<detail::EventBusState> state_;
    std::uint64_t                        id_ = 0;
    std::optional<std::string>           session_;
    Channel                              channel_ = Channel::Global;
};

class EventBus {
public:
    using Handler       = std::function<void(const Event&)>;
    // 24-D6 (spec 24 §6): a committed record carries the store `Sequence` the
    // append site assigned, so the daemon forwarder never re-reads the store
    // (AL16/AL17).
    using RecordHandler = std::function<void(const EventRecord&)>;

    EventBus();
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) noexcept = default;
    EventBus& operator=(EventBus&&) noexcept = default;

    // Global subscribers: logging, telemetry, the host event forwarder, and a
    // durable-only observer such as SessionStore (which filters to durable
    // EventTypes itself, §8.1).
    [[nodiscard]] Subscription subscribe(Handler handler);

    // Per-session subscribers: backed by that session's ordered mailbox. The
    // handler sees only events whose session_id == session.
    [[nodiscard]] Subscription subscribe(const SessionId& session, Handler handler);

    // Typed overloads (01 §5): the payload is erased to `Event` via encode().
    template <class Payload>
    [[nodiscard]] Subscription subscribe(std::function<void(const Payload&)> handler) {
        return subscribe(detail::erase_handler<Payload>(std::move(handler)));
    }

    template <class Payload>
    [[nodiscard]] Subscription subscribe(
        const SessionId& session,
        std::function<void(const Payload&)> handler) {
        return subscribe(session, detail::erase_handler<Payload>(std::move(handler)));
    }

    // The single publish entry point. Durable events reach the bus only after
    // they are committed (I4); live events are published directly.
    void publish(Event event);

    template <class Payload>
    void publish(const TypedEvent<Payload>& event) {
        publish(encode(event));
    }

    // Global committed-record subscribers (24-D6/24-D12). They receive the
    // `EventRecord` with its store `Sequence`; per-session mailbox fan-out is
    // preserved exactly as `publish` does it.
    [[nodiscard]] Subscription subscribeCommitted(RecordHandler handler);

    void publishCommitted(const EventRecord& record);

    // Non-owning; valid until the session's last subscriber is removed.
    [[nodiscard]] SessionMailbox* mailbox(const SessionId& session) noexcept;

private:
    std::shared_ptr<detail::EventBusState> state_;
};

} // namespace ymh

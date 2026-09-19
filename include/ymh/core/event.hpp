#pragma once

// Core, frontend-agnostic event model.
//
// This is the erased runtime event defined by 00-architecture.md §8.2 and
// 01-session.md §4.2–§4.6. It deliberately carries no frontend/UI type (D15,
// §20.22) and no store-local `Sequence`: the read side wraps it in
// `EventRecord` (§4.6). Strongly typed payload structs live in `ymh::payload`
// and are attached to this erased form only at the encode/decode boundary.
//
// The concrete durable payload set (01 §4.5) depends on the agent message
// types (§12), which arrive in a later wave. The generic seam
// (`EventTraits<P>` / `TypedEvent<P>` / `encode` / `decode`) is complete here
// so those payload specs can plug in without touching the bus.

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ymh {

// Store-assigned position in the event log. 0 means "not yet appended".
// Strictly increasing; assigned only by SessionStore (01 §2.1, I2).
using Sequence = std::int64_t;

// UUIDv4; stable; never a path (01 §2.1, §9.2).
struct SessionId {
    std::string value;

    auto operator<=>(const SessionId&) const = default;
};

// UUIDv4; globally unique (01 §2.1, I3).
struct EventId {
    std::string value;

    auto operator<=>(const EventId&) const = default;
};

// CamelCase enum; the JSON `type` string is the slash form (01 §4.3, §8.1).
// The durable set is non-exhaustive: component specs extend it, and this enum
// is the pinned extension owned by the session component.
enum class EventType : std::uint16_t {
    SessionStarted,      // wire: session/start
    SessionEnded,        // wire: session/end
    TurnStarted,         // wire: turn/start
    TurnEnded,           // wire: turn/end
    TurnCancelled,       // wire: turn/cancel
    TurnFailed,          // wire: turn/fail
    StepStarted,         // wire: step/start
    StepEnded,           // wire: step/end
    UserMessage,         // wire: user/message
    AssistantChunk,      // wire: assistant/chunk
    AssistantMessage,    // wire: assistant/message
    AssistantAttempt,    // wire: assistant/attempt (29-D2, 26-D9)
    ToolCall,            // wire: tool/call
    ToolResult,          // wire: tool/result
    PermissionDecision,  // wire: permission/decision
    ContextInjected,     // wire: context/injected
    ContextCompaction,   // wire: context/compaction
    TokenUsage,          // wire: usage
    SubagentSpawned,     // wire: subagent/spawned
    SubagentFanIn,       // wire: subagent/fan_in
    SessionRenamed,      // wire: session/renamed  (19 §5.1)
    PlanMode,            // wire: plan/mode       (25-D2)
    LlmRequestHeader,    // wire: llm/request_header (28 §5.2, 29 §3.2)
    // Live-only (15 §4.7, AM-1): never in the durable SessionEventMap, never
    // appended to the session log. Delivered to global EventBus subscribers.
    McpServerStatusChanged,  // wire: mcp/server_status_changed
};

// CamelCase -> slash form. Total over the enum; returns an empty view for a
// value outside the enum domain.
[[nodiscard]] std::string_view wire_name(EventType type) noexcept;

// Slash form -> enum. Rejects unknown strings (S3).
[[nodiscard]] std::optional<EventType> parse_event_type(std::string_view name) noexcept;

// Every EventType, in declaration order. Used by the exhaustive wire/encode
// round-trip tests (01 §15.1).
[[nodiscard]] std::span<const EventType> all_event_types() noexcept;

// The erased, frontend-agnostic event (00 §8.2, 01 §4.2).
struct Event {
    EventId                                id;
    SessionId                              session_id;
    std::chrono::system_clock::time_point  timestamp;
    EventType                              type;
    nlohmann::json                         payload;
};

// JSON is the persistence/wire representation (00 §8.2). The timestamp is
// encoded as epoch milliseconds; an unknown `type` string fails loudly (S3).
void to_json(nlohmann::json& json, const Event& event);
void from_json(const nlohmann::json& json, Event& event);

// Wire-only tolerant decode (29-D5, 29 §3.3 Axis B, 29-I3). Returns `nullopt`
// when `json["type"]` is not a known `EventType`, so a live-wire receiver can
// skip the event and advance its cursor instead of dropping the connection.
// Malformed JSON for a *known* type still throws: the durable/on-disk axis
// stays loud and never calls this entry point.
[[nodiscard]] std::optional<Event> try_decode_event(const nlohmann::json& json);

// The read side wraps the erased event with the store-assigned sequence,
// because `Event` itself carries no `Sequence` (01 §4.6).
struct EventRecord {
    Sequence seq;
    Event    event;
};

using EventRange = std::vector<EventRecord>;   // ascending by seq

// ---------------------------------------------------------------------------
// Typed payload seam (01 §4.4)
// ---------------------------------------------------------------------------

// EventType -> payload type. Specializations arrive with the payload specs.
template <EventType Type>
struct SessionEventMap;

template <EventType Type>
using session_payload_t = typename SessionEventMap<Type>::type;

// Payload type -> EventType. One specialization per payload:
//
//   template <> struct EventTraits<MyPayload> {
//       static constexpr EventType type = EventType::MyEvent;
//   };
template <class Payload>
struct EventTraits;

template <class Payload>
inline constexpr EventType event_type_v = EventTraits<Payload>::type;

// A typed event: the same metadata as `Event`, with a strongly typed payload.
template <class Payload>
struct TypedEvent {
    EventId                                id;
    SessionId                              session_id;
    std::chrono::system_clock::time_point  timestamp;
    Payload                                payload;
};

// Erase the typed payload into the wire/persistence form (00 §8.2).
template <class Payload>
[[nodiscard]] Event encode(const TypedEvent<Payload>& typed) {
    Event event;
    event.id         = typed.id;
    event.session_id = typed.session_id;
    event.timestamp  = typed.timestamp;
    event.type       = EventTraits<Payload>::type;
    event.payload    = typed.payload;   // ADL to_json
    return event;
}

// Rehydrate a typed payload from the erased form. The caller is responsible
// for `event.type == EventTraits<Payload>::type`; a mismatched or malformed
// payload fails loudly via from_json (S3).
template <class Payload>
[[nodiscard]] TypedEvent<Payload> decode(const Event& event) {
    return TypedEvent<Payload>{
        event.id,
        event.session_id,
        event.timestamp,
        event.payload.get<Payload>(),   // ADL from_json
    };
}

} // namespace ymh

// ---------------------------------------------------------------------------
// Standard specializations pinned by 01 §2.1
// ---------------------------------------------------------------------------

namespace std {

template <>
struct hash<ymh::SessionId> {
    [[nodiscard]] std::size_t operator()(const ymh::SessionId& id) const noexcept {
        return std::hash<std::string>{}(id.value);
    }
};

template <>
struct hash<ymh::EventId> {
    [[nodiscard]] std::size_t operator()(const ymh::EventId& id) const noexcept {
        return std::hash<std::string>{}(id.value);
    }
};

template <>
struct formatter<ymh::SessionId, char> : formatter<std::string, char> {
    auto format(const ymh::SessionId& id, std::format_context& ctx) const {
        return formatter<std::string, char>::format(id.value, ctx);
    }
};

template <>
struct formatter<ymh::EventId, char> : formatter<std::string, char> {
    auto format(const ymh::EventId& id, std::format_context& ctx) const {
        return formatter<std::string, char>::format(id.value, ctx);
    }
};

} // namespace std

#pragma once

// Agent identity, extracted so a durable payload can name an owner without
// pulling in the full `Agent` handle (which transitively includes
// `session/events.hpp`). Pinned by 06-agent-loop.md §2; the `Agent` interface
// and its value types remain in `agent/agent.hpp`.

#include <compare>
#include <string>

namespace ymh {

// UUIDv4; globally unique (06 §2.1).
struct AgentId {
    std::string value;

    auto operator<=>(const AgentId&) const = default;
};

} // namespace ymh

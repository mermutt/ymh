#pragma once

// Session-local and provider correlation identifiers pinned by
// 01-session.md §2.1. `SessionId` / `EventId` / `Sequence` live in
// `ymh/core/event.hpp`; these complete the §2.1 set without touching core.

#include <cstdint>
#include <string>

namespace ymh {

// Session-local monotonic counters, assigned by the session on append so a
// replayed log reconstructs identical turn/step identities (01 §2.1).
using TurnId = std::uint64_t;
using StepId = std::uint64_t;

// Provider-supplied correlation ids; must round-trip verbatim (01 §2.1, §12).
using ToolCallId = std::string;
using MessageId  = std::string;

} // namespace ymh

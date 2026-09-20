#pragma once

// The log-only command value types, pinned by 44-goals-jobs-commands.md §5.6
// (26 §4.3.8). A command invocation appends two durable events paired by
// `command_id`; the events are ignored by deriveMessages and token accounting
// (44-I12). This header carries the value vocabulary the `/goal` handler uses;
// the registry that emits the events is the D20 surface.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "ymh/agent/agent.hpp"
#include "ymh/commands/command_types.hpp"
#include "ymh/core/event.hpp"

namespace ymh {

struct CommandInput {
    std::string   name;        // without the leading '/'
    std::string   raw_input;   // verbatim, separator whitespace included
    SessionId     session;
    AgentId       agent;
    CommandSource source = CommandSource::User;
};

// 26 §4.3.8; dsh CommandResult (types.d.ts:33-41).
struct CommandOutcome {
    CommandOutcomeKind          kind = CommandOutcomeKind::Success;
    std::string                 text;               // rendered outcome / error text
    std::optional<Sequence>     source_event_seq;   // richer domain event
};

using CommandHandler = std::function<CommandOutcome(const CommandInput&, Agent&)>;

// 26 §4.3.8 + the recordInput flag (dsh types.d.ts:90-97).
struct CommandSpec {
    std::string                name;
    std::string                description;
    std::optional<std::string> input_hint;
    bool                       record_input = true;
    CommandHandler             handler;
};

// Handler-free view for discovery UI (dsh types.d.ts:55-62).
struct CommandDescriptor {
    std::string                name;
    std::string                description;
    std::optional<std::string> input_hint;
};

} // namespace ymh

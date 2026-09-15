#pragma once

// Synchronous v1 subagent spawn/fan-in (06-agent-loop.md §7, decision (i)). A
// subagent is an ordinary `Agent` with its own session and log; the parent
// records only the `SubagentSpawned`/`SubagentFanIn` edges (A16/F11). The
// spawning capability awaits the child's terminal state via `whenIdle()`.

#include <string>

#include "ymh/agent/agent_registry.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {

class SubagentRunner {
public:
    SubagentRunner(AgentRegistry& registry,
                   SessionManager& sessions,
                   Session& parent,
                   SessionOptions options);

    payload::SubagentOutcome run(const std::string& task, std::string& summary);

private:
    AgentRegistry& registry_;
    SessionManager& sessions_;
    Session&        parent_;
    SessionOptions  options_;
};

} // namespace ymh

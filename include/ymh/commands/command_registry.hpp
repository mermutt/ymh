#pragma once

// The host-side, agent-scoped, log-only command registry, pinned by
// 44-goals-jobs-commands.md §5.6 (26-D20). `invoke` appends `command/run`
// before the handler and `command/done` at settlement; both are ignored by
// deriveMessages and token accounting (44-I12). Resolution walks the roster's
// scope chain nearest-first (44-I14). Single-threaded (44-I17).

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/preset.hpp"
#include "ymh/commands/command.hpp"

namespace ymh {

class SessionManager;

using ScopeParent = std::function<std::optional<ScopeKey>(const ScopeKey&)>;
using ScopeFor    = std::function<ScopeKey(const Agent&)>;

class CommandRegistry {
public:
    explicit CommandRegistry(SessionManager& sessions, ScopeFor scope_for = {},
                             ScopeParent parent = {});

    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    void add(CommandSpec spec, const ScopeKey& scope);
    [[nodiscard]] std::vector<CommandDescriptor> list(const AgentContext& context) const;
    [[nodiscard]] const CommandSpec* resolve(const AgentContext& context,
                                             std::string_view    name) const;
    CommandOutcome invoke(Agent& agent, std::string_view line, CommandSource source);

    [[nodiscard]] std::vector<ScopeKey> chain(const AgentContext& context) const;

private:
    SessionManager*                                        sessions_ = nullptr;
    ScopeFor                                               scope_for_;
    ScopeParent                                            parent_;
    std::map<ScopeKey, std::map<std::string, CommandSpec>> scopes_;
    CommandId                                              next_command_id_ = 0;
};

} // namespace ymh

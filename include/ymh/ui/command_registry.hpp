#pragma once

// Supervisor-local slash-command registry (10 §8.2 refinement). Commands are a
// UI-only affordance: a handler mutates the presentation model through the
// `CommandContext` callbacks and never touches the wire or the event log.

#include <functional>
#include <string>
#include <vector>

#include "ymh/ui/ui_model.hpp"

namespace ymh::ui {

struct CommandContext {
    explicit CommandContext(UiModel& model) : model(model) {}

    UiModel&                                model;
    SessionUiState*                         session = nullptr;
    std::function<void()>                   request_exit;
    std::function<void()>                   create_session;
    std::function<void(const std::string&)> set_model;
    std::function<void()>                   compact;
};

struct Command {
    std::string name;   // without the leading '/'
    std::string description;
    std::function<void(CommandContext&, const std::string& args)> handler;
};

class CommandRegistry {
public:
    void add(Command command);
    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }
    [[nodiscard]] const Command* find(const std::string& name) const;
    [[nodiscard]] std::vector<const Command*> complete(const std::string& prefix) const;
    // True when `line` is a command line (leading '/'), recognized or not. A
    // system notice is appended either way; non-command lines return false.
    bool dispatch(const std::string& line, CommandContext& context) const;

    [[nodiscard]] static CommandRegistry builtin();

private:
    std::vector<Command> commands_;
};

} // namespace ymh::ui

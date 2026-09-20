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
    // 19 §6.1: appends a User rename through the daemon; `args` is the trimmed
    // title. A rejected rename is surfaced by the wired reply lambda.
    std::function<void(const std::string&)> rename_session;
    // Serializes the active session transcript to a markdown file; `args` is the
    // raw command-line tail (`[path] [--edit]`). Returns the notice to surface.
    std::function<std::string(const std::string&)> export_session;
    // 20 §5.8: `/skills [--show NAME]` lists discovered skills (read-only).
    std::function<void(const std::string& args)> skills;
    // 25-D5: `active` selects plan mode; a non-empty `message` is steered as one
    // user message after the selection (so it is composed under plan guidance).
    std::function<void(bool active, const std::string& message)> plan_mode;
    // 18 §4.1: opens the read-only context overlay for the active session.
    std::function<void()> context;
    // 22 §4.3/§4.6 (S2): opens the History catalog overlay (`/sessions`).
    std::function<void()> sessions;
};

// 20 §6.1: the non-static, callable-from-supervisor form of the file-local
// `append_system`. Appends a System conversation entry and marks it dirty.
void append_system_entry(UiModel& model, SessionUiState& state, std::string text);

struct Command {
    std::string name;   // without the leading '/'
    std::string description;
    std::function<void(CommandContext&, const std::string& args)> handler;
    std::vector<std::string> aliases{};   // 25-D12
};

// 45-D8: pure display-name helper. Returns `name(alias1,alias2)` when aliases
// are present, else `name`; for `/exit` it returns "exit(quit)".
[[nodiscard]] std::string command_display_name(const Command& command);

class CommandRegistry {
public:
    void add(Command command);
    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }
    [[nodiscard]] const Command* find(const std::string& name) const;
    [[nodiscard]] std::vector<const Command*> complete(const std::string& prefix) const;
    [[nodiscard]] static std::string longest_common_prefix(
        const std::vector<const Command*>& commands);
    // True when `line` is a command line (leading '/'), recognized or not. A
    // system notice is appended either way; non-command lines return false.
    bool dispatch(const std::string& line, CommandContext& context) const;

    [[nodiscard]] static CommandRegistry builtin();

private:
    std::vector<Command> commands_;
};

} // namespace ymh::ui

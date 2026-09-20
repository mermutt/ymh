#include "ymh/ui/command_registry.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ymh::ui {
namespace {

std::string trim(const std::string& input) {
    std::size_t begin = 0;
    while (begin < input.size() &&
           std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    std::size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(begin, end - begin);
}

void append_system(CommandContext& context, std::string text) {
    if (context.session == nullptr) {
        return;
    }
    append_system_entry(context.model, *context.session, std::move(text));
}

std::string command_names(const CommandRegistry& registry) {
    std::string names;
    for (const Command& command : registry.commands()) {
        if (!names.empty()) {
            names += ", ";
        }
        names += "/" + command.name;
    }
    return names;
}

} // namespace

void append_system_entry(UiModel& model, SessionUiState& state, std::string text) {
    ConversationEntry entry;
    entry.role = ConversationRole::System;
    entry.text = std::move(text);
    state.conversation.entries.push_back(std::move(entry));
    state.scroll.onNewContent();
    model.dirty.mark(state.id, UiDirtyFlag::Conversation);
}

std::string command_display_name(const Command& command) {
    if (command.aliases.empty()) {
        return command.name;
    }
    std::string display = command.name + "(";
    for (std::size_t index = 0; index < command.aliases.size(); ++index) {
        if (index != 0) {
            display += ",";
        }
        display += command.aliases[index];
    }
    display += ")";
    return display;
}

void CommandRegistry::add(Command command) {
    commands_.push_back(std::move(command));
}

const Command* CommandRegistry::find(const std::string& name) const {
    const auto by_name = std::find_if(commands_.begin(), commands_.end(),
                                      [&name](const Command& command) {
                                          return command.name == name;
                                      });
    if (by_name != commands_.end()) {
        return &*by_name;
    }
    const auto by_alias = std::find_if(commands_.begin(), commands_.end(),
                                       [&name](const Command& command) {
                                           return std::find(command.aliases.begin(),
                                                            command.aliases.end(),
                                                            name) != command.aliases.end();
                                       });
    return by_alias == commands_.end() ? nullptr : &*by_alias;
}

std::vector<const Command*> CommandRegistry::complete(const std::string& prefix) const {
    std::vector<const Command*> matches;
    for (const Command& command : commands_) {
        if (command.name.size() >= prefix.size() &&
            command.name.compare(0, prefix.size(), prefix) == 0) {
            matches.push_back(&command);
        }
    }
    return matches;
}

std::string CommandRegistry::longest_common_prefix(
    const std::vector<const Command*>& commands) {
    if (commands.empty()) {
        return {};
    }
    std::string prefix = commands.front()->name;
    for (const Command* command : commands) {
        std::size_t length = 0;
        while (length < prefix.size() && length < command->name.size() &&
               prefix[length] == command->name[length]) {
            ++length;
        }
        prefix.resize(length);
        if (prefix.empty()) {
            break;
        }
    }
    return prefix;
}

bool CommandRegistry::dispatch(const std::string& line, CommandContext& context) const {
    if (line.empty() || line.front() != '/') {
        return false;
    }
    const std::string body = trim(line.substr(1));
    const std::size_t split = body.find_first_of(" \t");
    const std::string name = split == std::string::npos ? body : body.substr(0, split);
    const std::string args = split == std::string::npos ? std::string{} : trim(body.substr(split + 1));

    if (name.empty()) {
        append_system(context, "commands: " + command_names(*this));
        return true;
    }
    const Command* command = find(name);
    if (command == nullptr) {
        append_system(context, "unknown command: /" + name + " (try /help)");
        return true;
    }
    command->handler(context, args);
    return true;
}

CommandRegistry CommandRegistry::builtin() {
    CommandRegistry registry;
    registry.add(Command{
        "new", "create and activate a new session",
        [](CommandContext& context, const std::string&) {
            if (context.create_session) {
                context.create_session();
            }
            append_system(context, "creating a new session");
        }});
    registry.add(Command{
        "clear", "clear the conversation view (the log is kept)",
        [](CommandContext& context, const std::string&) {
            if (context.session == nullptr) {
                return;
            }
            context.session->conversation.entries.clear();
            context.session->conversation.by_message.clear();
            context.session->conversation.by_reasoning_message.clear();
            context.session->scroll.toBottom();
            context.model.dirty.mark(context.session->id, UiDirtyFlag::Conversation);
        }});
    registry.add(Command{
        "model", "show or set the model (applies to new sessions)",
        [](CommandContext& context, const std::string& args) {
            if (args.empty()) {
                const std::string current =
                    context.session == nullptr ? std::string{} : context.session->status.model;
                append_system(context, "model: " + (current.empty() ? std::string{"(default)"} : current));
                return;
            }
            if (context.set_model) {
                context.set_model(args);
            }
            if (context.session != nullptr) {
                context.session->status.model = args;
                context.model.dirty.mark(context.session->id, UiDirtyFlag::Status);
            }
            append_system(context,
                          "model set to " + args +
                              " (the daemon applies it at session.create; there is no "
                              "live model switch)");
        }});
    registry.add(Command{
        "compact", "Summarize history to reclaim context",
        [](CommandContext& context, const std::string&) {
            if (context.compact) {
                context.compact();
            }
            append_system(context, "compaction requested");
        }});
    registry.add(Command{
        "rename", "rename the current session",
        [](CommandContext& context, const std::string& args) {
            if (context.session == nullptr) {
                append_system(context, "no active session");
                return;
            }
            if (args.empty()) {
                append_system(context, "usage: /rename <title>");
                return;
            }
            if (context.rename_session) {
                context.rename_session(args);
            }
        }});
    registry.add(Command{
        "export", "write the session transcript to a markdown file (--edit to open it)",
        [](CommandContext& context, const std::string& args) {
            if (!context.export_session) {
                append_system(context, "export is unavailable");
                return;
            }
            const std::string notice = context.export_session(args);
            if (!notice.empty()) {
                append_system(context, notice);
            }
        }});
    registry.add(Command{
        "skills", "list available skills",
        [](CommandContext& context, const std::string& args) {
            if (context.skills) {
                context.skills(args);
            }
        }});
    registry.add(Command{
        "context", "show the assembled context window (grid, tools, MCP)",
        [](CommandContext& context, const std::string&) {
            if (context.context) {
                context.context();
            }
        }});
    registry.add(Command{
        "sessions", "list stored sessions for every workspace",
        [](CommandContext& context, const std::string&) {
            if (context.sessions) {
                context.sessions();
            }
        }});
    registry.add(Command{
        "mcp", "list configured MCP servers and their tools",
        [](CommandContext& context, const std::string&) {
            if (context.mcp) {
                context.mcp();
            }
        }});
    registry.add(Command{
        "status", "show version, model, API connectivity, tools, and MCP state",
        [](CommandContext& context, const std::string&) {
            if (context.status) {
                context.status();
            }
        }});
    registry.add(Command{
        "plan", "enter plan mode (/plan off to leave)",
        [](CommandContext& context, const std::string& args) {
            if (context.session == nullptr) {
                append_system(context, "no active session");
                return;
            }
            if (!context.plan_mode) {
                return;
            }
            if (args.empty()) {
                context.plan_mode(true, std::string{});
            } else if (args == "off") {
                context.plan_mode(false, std::string{});
            } else {
                context.plan_mode(true, args);
            }
        }});
    registry.add(Command{
        "exit", "quit the supervisor",
        [](CommandContext& context, const std::string&) {
            if (context.request_exit) {
                context.request_exit();
            }
        },
        {"quit"}});
    std::vector<std::pair<std::string, std::string>> listed{{"help", "list slash commands"}};
    for (const Command& command : registry.commands()) {
        listed.emplace_back(command_display_name(command), command.description);
    }
    registry.add(Command{
        "help", "list slash commands",
        [listed = std::move(listed)](CommandContext& context, const std::string&) {
            append_system(context, "commands:");
            for (const auto& [display, description] : listed) {
                append_system(context, "  /" + display + " - " + description);
            }
        }});
    return registry;
}

} // namespace ymh::ui

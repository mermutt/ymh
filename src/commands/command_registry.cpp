#include "ymh/commands/command_registry.hpp"

#include <exception>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {
namespace {

CommandOutcome failure(std::string text) {
    return CommandOutcome{CommandOutcomeKind::Error, std::move(text), std::nullopt};
}

} // namespace

CommandRegistry::CommandRegistry(SessionManager& sessions, ScopeFor scope_for, ScopeParent parent)
    : sessions_(&sessions), scope_for_(std::move(scope_for)), parent_(std::move(parent)) {}

void CommandRegistry::add(CommandSpec spec, const ScopeKey& scope) {
    std::map<std::string, CommandSpec>& layer = scopes_[scope];
    if (layer.find(spec.name) != layer.end()) {
        throw std::invalid_argument("duplicate command '" + spec.name + "' in one scope");
    }
    layer.emplace(spec.name, std::move(spec));
}

std::vector<ScopeKey> CommandRegistry::chain(const AgentContext& context) const {
    std::vector<ScopeKey> out;
    std::set<ScopeKey>    seen;
    ScopeKey              scope = context.scope;
    while (seen.insert(scope).second) {
        out.push_back(scope);
        if (!parent_) {
            break;
        }
        const std::optional<ScopeKey> next = parent_(scope);
        if (!next.has_value()) {
            break;
        }
        scope = *next;
    }
    return out;
}

const CommandSpec* CommandRegistry::resolve(const AgentContext& context,
                                            std::string_view    name) const {
    for (const ScopeKey& scope : chain(context)) {
        const auto layer = scopes_.find(scope);
        if (layer == scopes_.end()) {
            continue;
        }
        const auto found = layer->second.find(std::string{name});
        if (found != layer->second.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

std::vector<CommandDescriptor> CommandRegistry::list(const AgentContext& context) const {
    std::map<std::string, CommandDescriptor> visible;
    for (const ScopeKey& scope : chain(context)) {
        const auto layer = scopes_.find(scope);
        if (layer == scopes_.end()) {
            continue;
        }
        for (const auto& [name, spec] : layer->second) {
            if (visible.find(name) == visible.end()) {
                visible.emplace(name, CommandDescriptor{spec.name, spec.description, spec.input_hint});
            }
        }
    }
    std::vector<CommandDescriptor> out;
    out.reserve(visible.size());
    for (auto& entry : visible) {
        out.push_back(std::move(entry.second));
    }
    return out;
}

CommandOutcome CommandRegistry::invoke(Agent& agent, std::string_view line, CommandSource source) {
    std::string_view text = line;
    if (!text.empty() && text.front() == '/') {
        text.remove_prefix(1);
    }
    const std::size_t separator = text.find_first_of(" \t");
    const std::string name =
        separator == std::string_view::npos ? std::string{text} : std::string{text.substr(0, separator)};
    std::string raw_input;
    if (separator != std::string_view::npos) {
        std::size_t begin = separator;
        while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
            ++begin;
        }
        raw_input = std::string{text.substr(begin)};
    }
    if (name.empty()) {
        return failure("empty command");
    }

    const ScopeKey scope = scope_for_ ? scope_for_(agent) : ScopeKey{};
    const AgentContext context{agent.id(), scope};
    const CommandSpec* spec = resolve(context, name);
    if (spec == nullptr) {
        return failure("unknown command: " + name);
    }

    const CommandId command_id = ++next_command_id_;
    const std::shared_ptr<Session> session = sessions_->sessionPtr(agent.session());

    payload::CommandRun run;
    run.command_id = command_id;
    run.name       = name;
    if (spec->record_input) {
        run.args = raw_input;
    }
    run.source = source;
    session->append(run);

    const CommandInput input{name, raw_input, agent.session(), agent.id(), source};
    CommandOutcome     outcome;
    try {
        outcome = spec->handler(input, agent);
    } catch (const std::exception& error) {
        outcome = failure(error.what());
    } catch (...) {
        outcome = failure("command handler failed");
    }

    payload::CommandDone done;
    done.command_id = command_id;
    done.kind = outcome.kind == CommandOutcomeKind::Error ? payload::CommandDoneKind::Error
                                                         : payload::CommandDoneKind::Success;
    if (!outcome.text.empty()) {
        done.text = outcome.text;
    }
    done.source_event_seq = outcome.source_event_seq;
    session->append(done);
    return outcome;
}

} // namespace ymh

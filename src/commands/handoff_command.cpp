#include "ymh/handoff/handoff_command.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/handoff.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {
namespace {

std::string trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(begin, end - begin + 1)};
}

bool parse_handoff_args(std::string_view args, HandoffOptions& options, std::string& bad_option) {
    std::size_t index = 0;
    while (index < args.size()) {
        while (index < args.size() && (args[index] == ' ' || args[index] == '\t')) {
            ++index;
        }
        if (index >= args.size()) {
            break;
        }
        const std::size_t start = index;
        while (index < args.size() && args[index] != ' ' && args[index] != '\t') {
            ++index;
        }
        const std::string token{args.substr(start, index - start)};
        if (token == "--no-seed") {
            options.seed = false;
            continue;
        }
        if (token == "--out") {
            while (index < args.size() && (args[index] == ' ' || args[index] == '\t')) {
                ++index;
            }
            if (index >= args.size()) {
                bad_option = token;
                return false;
            }
            const std::size_t value_start = index;
            while (index < args.size() && args[index] != ' ' && args[index] != '\t') {
                ++index;
            }
            options.out = std::string{args.substr(value_start, index - value_start)};
            continue;
        }
        if (token == "--title") {
            while (index < args.size() && (args[index] == ' ' || args[index] == '\t')) {
                ++index;
            }
            options.title = trim(args.substr(index));
            index         = args.size();
            continue;
        }
        bad_option = token;
        return false;
    }
    return true;
}

CommandOutcome failure(std::string text) {
    return CommandOutcome{CommandOutcomeKind::Error, std::move(text), std::nullopt};
}

CommandOutcome success(std::string text) {
    return CommandOutcome{CommandOutcomeKind::Success, std::move(text), std::nullopt};
}

} // namespace

CommandOutcome run_handoff_command(HandoffService& service, const CommandInput& input,
                                   Agent& agent) {
    HandoffOptions options;
    std::string    bad_option;
    if (!parse_handoff_args(input.raw_input, options, bad_option)) {
        return failure("handoff: unknown option " + bad_option);
    }
    std::shared_ptr<Session> session = service.sessions().sessionPtr(agent.session());
    if (session == nullptr) {
        return failure("handoff: unknown session");
    }
    const std::vector<Message> view   = session->deriveMessages();
    const HandoffResult        result = service.run(*session, view, options, CancellationToken{});
    if (result.outcome == HandoffResult::Outcome::Ok) {
        return success(result.detail);
    }
    return failure(result.detail.empty() ? std::string{"handoff: failed"} : result.detail);
}

CommandSpec make_handoff_command(HandoffService& service) {
    CommandSpec spec;
    spec.name        = "handoff";
    spec.description = "summarize this session for continuation (writes a doc, seeds a session)";
    spec.input_hint  = "[--no-seed] [--out <path>] [--title <text>]";
    spec.handler     = [&service](const CommandInput& input, Agent& agent) {
        return run_handoff_command(service, input, agent);
    };
    return spec;
}

} // namespace ymh

#include "support/replay_harness.hpp"

#include <cstddef>
#include <utility>

namespace ymh::test {
namespace {

GenerationParameters parameters_from(const LlmCallConfig& config) {
    GenerationParameters parameters;
    parameters.temperature      = config.temperature;
    parameters.top_p            = config.top_p;
    parameters.max_output_tokens = config.max_tokens;
    parameters.stop             = config.stop;
    parameters.tool_choice      = config.tool_choice;
    parameters.reasoning_effort = config.reasoning_effort;
    parameters.seed             = config.seed;
    return parameters;
}

std::string system_text(const payload::LlmRequestHeader& header, const ReplayEnv& env) {
    if (header.system_prompt.has_value()) {
        return *header.system_prompt;
    }
    if (env.rendered_system_prompt.has_value()) {
        return *env.rendered_system_prompt;
    }
    return {};
}

bool prompt_known(const payload::LlmRequestHeader& header, const ReplayEnv& env) {
    return header.system_prompt.has_value() || env.rendered_system_prompt.has_value();
}

void prepend_system(std::vector<Message>& messages, const std::string& prompt) {
    if (prompt.empty()) {
        return;
    }
    Message system;
    system.role = Role::System;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = prompt;
    system.content.push_back(std::move(block));
    messages.insert(messages.begin(), std::move(system));
}

void validate_prefix(const EventRange& log, const EventRange& prefix, Sequence settlement_seq) {
    bool settlement_found = false;
    for (const EventRecord& record : log) {
        if (record.seq != settlement_seq) {
            continue;
        }
        if (record.event.type != EventType::AssistantMessage &&
            record.event.type != EventType::AssistantAttempt) {
            throw ReplayMismatch(ReplayMismatchKind::PrefixMismatch,
                                 "settlement_seq is not an attempt settlement record");
        }
        settlement_found = true;
        break;
    }
    if (!settlement_found) {
        throw ReplayMismatch(ReplayMismatchKind::PrefixMismatch,
                             "settlement_seq is not present in the log");
    }
    if (prefix.size() > log.size()) {
        throw ReplayMismatch(ReplayMismatchKind::PrefixMismatch, "prefix is longer than the log");
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (prefix[index].seq != log[index].seq) {
            throw ReplayMismatch(ReplayMismatchKind::PrefixMismatch,
                                 "prefix is not log[0..k)");
        }
    }
    if (prefix.size() >= log.size() || log[prefix.size()].seq != settlement_seq) {
        throw ReplayMismatch(ReplayMismatchKind::PrefixMismatch,
                             "prefix does not reach the attempt's settlement boundary");
    }
}

std::vector<ToolSchema> registry_tools(const ReplayEnv& env) {
    if (env.registry == nullptr) {
        throw ReplayMismatch(ReplayMismatchKind::MissingHeader, "no tool registry supplied");
    }
    return env.registry->schemas();
}

FrozenRequest build_full_request(const std::vector<Message>& messages,
                                 const std::vector<ToolSchema>& tools,
                                 const payload::LlmRequestHeader& header) {
    LLMRequest request;
    request.model      = header.config.model;
    request.messages   = messages;
    request.tools      = tools;
    request.session_id = header.session_id;
    request.purpose    = header.purpose;
    request.parameters = parameters_from(header.config);
    return FrozenRequest::freeze(std::move(request), header.config);
}

} // namespace

FrozenRequest rebuild_template(const payload::LlmRequestHeader& header, const ReplayEnv& env) {
    const std::vector<ToolSchema> tools = registry_tools(env);

    if (tools.size() != header.tool_names.size()) {
        throw ReplayMismatch(ReplayMismatchKind::ToolNameSetMismatch,
                             "registry tool count differs from header.tool_names");
    }
    for (std::size_t index = 0; index < tools.size(); ++index) {
        if (tools[index].name.value != header.tool_names[index]) {
            throw ReplayMismatch(ReplayMismatchKind::ToolNameSetMismatch,
                                 "registry tool name differs from header.tool_names[" +
                                     std::to_string(index) + "]");
        }
    }
    if (tools.size() != header.tool_schema_digests.size()) {
        throw ReplayMismatch(ReplayMismatchKind::ToolSchemaMismatch,
                             "registry tool count differs from header.tool_schema_digests");
    }
    for (std::size_t index = 0; index < tools.size(); ++index) {
        if (tool_schema_digest(tools[index]) != header.tool_schema_digests[index]) {
            throw ReplayMismatch(ReplayMismatchKind::ToolSchemaMismatch,
                                 "tool schema digest differs at index " +
                                     std::to_string(index));
        }
    }

    const std::string prompt = system_text(header, env);
    LLMRequest        request;
    request.model      = header.config.model;
    request.session_id = header.session_id;
    request.purpose    = header.purpose;
    request.tools      = tools;
    request.parameters = parameters_from(header.config);
    prepend_system(request.messages, prompt);

    FrozenRequest rebuilt = FrozenRequest::freeze(std::move(request), header.config);
    if (prompt_known(header, env)) {
        if (sha256_hex(prompt) != header.system_prompt_digest) {
            throw ReplayMismatch(ReplayMismatchKind::PromptDigestMismatch,
                                 "rendered system prompt digest differs from the header");
        }
        if (rebuilt.template_digest() != header.template_digest) {
            throw ReplayMismatch(ReplayMismatchKind::TemplateDigestMismatch,
                                 "rebuilt template digest differs from the header");
        }
    }
    return rebuilt;
}

std::vector<Message> reconstruct_messages(const SessionHeader& session_header,
                                          const EventRange& log,
                                          const EventRange& prefix,
                                          Sequence settlement_seq,
                                          const payload::LlmRequestHeader& header,
                                          const ReplayEnv& env) {
    validate_prefix(log, prefix, settlement_seq);

    // 34 §9.2 step 2: the LAST `LlmRequestHeader` with `(turn, step)` no later
    // than the attempt's, positionally preceding the dispatch. The caller passes
    // the header it believes identifies the attempt; it must match the located
    // record's coordinates (a mismatched candidate is an unreconstructable
    // header).
    std::optional<payload::LlmRequestHeader> located;
    for (const EventRecord& record : prefix) {
        if (record.event.type != EventType::LlmRequestHeader) {
            continue;
        }
        const auto& candidate = record.event.payload.get<payload::LlmRequestHeader>();
        if (candidate.turn < header.turn ||
            (candidate.turn == header.turn && candidate.step <= header.step)) {
            located = candidate;
        }
    }
    if (!located.has_value()) {
        throw ReplayMismatch(ReplayMismatchKind::MissingHeader,
                             "no LlmRequestHeader precedes the attempt");
    }
    if (located->turn != header.turn || located->step != header.step ||
        located->session_id != header.session_id) {
        throw ReplayMismatch(ReplayMismatchKind::MissingHeader,
                             "caller-supplied header does not match the located "
                             "LlmRequestHeader");
    }

    std::vector<Message> messages = deriveMessages(session_header, prefix);
    prepend_system(messages, system_text(header, env));
    return messages;
}

ReplayReport assert_reconstructable(const SessionHeader& session_header,
                                    const EventRange& log,
                                    const EventRange& prefix,
                                    Sequence settlement_seq,
                                    const payload::LlmRequestHeader& header,
                                    const ReplayEnv& env) {
    validate_prefix(log, prefix, settlement_seq);
    const std::vector<ToolSchema> tools = registry_tools(env);

    std::vector<Message> first  = reconstruct_messages(
        session_header, log, prefix, settlement_seq, header, env);
    std::vector<Message> second = reconstruct_messages(
        session_header, log, prefix, settlement_seq, header, env);

    FrozenRequest rebuilt_first  = build_full_request(first, tools, header);
    FrozenRequest rebuilt_second = build_full_request(second, tools, header);
    if (rebuilt_first.canonical_json() != rebuilt_second.canonical_json()) {
        throw ReplayMismatch(ReplayMismatchKind::MessageDivergence,
                             "two rebuilds of canonical_json() differ");
    }

    // 34 §9.3/§9.4: the tool name/schema checks are prompt-independent, so they
    // run even when the prompt text is unavailable. Only the prompt-dependent
    // digest checks inside `rebuild_template` are skipped in that mode.
    static_cast<void>(rebuild_template(header, env));

    if (!prompt_known(header, env)) {
        return ReplayReport{ReplayStatus::PromptUnavailable,
                            std::move(rebuilt_first),
                            rebuilt_first.canonical_json()};
    }

    return ReplayReport{ReplayStatus::Verified,
                        std::move(rebuilt_first),
                        rebuilt_first.canonical_json()};
}

} // namespace ymh::test

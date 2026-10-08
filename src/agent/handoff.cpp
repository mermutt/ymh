#include "ymh/agent/handoff.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include "ymh/agent/llm_pool.hpp"
#include "ymh/agent/model_selection.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/llm/llm_call_config.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/stream.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {
namespace {

constexpr std::string_view kHandoffInstruction = R"PROMPT(You are preparing a handoff of this AI coding session so another model can continue the work with no loss of essential context.

Output EXACTLY the Markdown structure below: keep every section, in order. Use terse bullets, not prose paragraphs. Write "(none)" for an empty section - never drop a section.

## Goal
- [the user's original and evolving intent; quote verbatim where the exact wording matters]

## Key Decisions
- [decisions made and their rationale, including corrections the user requested]

## Current State
- [precisely what is done and where the work stands]

## Next Steps
- [the single next action first, then any queued follow-ups]

## Open Questions
- [unresolved choices, blockers, and assumptions]

## Relevant Files
- [exact workspace-relative paths that matter]

Rules:
- Write concise English engineering prose. Preserve exact file paths, commands, error strings, identifiers, numeric values, function signatures, and syntax fragments.
- Capture user feedback and explicit instructions faithfully, especially corrections.
- Do NOT mention this summarization request or that the context was compacted.
- Output only the six-section Markdown: do not call any tool or take any other action.)PROMPT";

constexpr std::array<std::string_view, 6> kHandoffHeadings{{
    "## Goal",
    "## Key Decisions",
    "## Current State",
    "## Next Steps",
    "## Open Questions",
    "## Relevant Files",
}};

std::string format_stamp(std::time_t utc_now) {
    std::tm tm{};
    gmtime_r(&utc_now, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return out.str();
}

std::string format_utc(std::time_t utc_now) {
    std::tm tm{};
    gmtime_r(&utc_now, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S UTC");
    return out.str();
}

std::string to_ascii(std::string text) {
    for (char& character : text) {
        if (static_cast<unsigned char>(character) >= 0x80u) {
            character = '?';
        }
    }
    return text;
}

std::size_t utf8_prefix(const std::string& text, std::size_t limit) {
    if (limit >= text.size()) {
        return text.size();
    }
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return cut;
}

std::string cap_title(const std::string& title, std::size_t max_bytes) {
    if (title.size() <= max_bytes) {
        return title;
    }
    return title.substr(0, utf8_prefix(title, max_bytes));
}

std::string join_text_blocks(const std::vector<ContentBlock>& blocks) {
    std::string out;
    for (const ContentBlock& block : blocks) {
        if (block.kind != ContentBlockKind::Text || block.text.empty()) {
            continue;
        }
        if (!out.empty()) {
            out.push_back('\n');
        }
        out += block.text;
    }
    return out;
}

std::string join_assistant_text(const std::vector<ContentBlock>& blocks) {
    std::string out;
    for (const ContentBlock& block : blocks) {
        if ((block.kind != ContentBlockKind::Text && block.kind != ContentBlockKind::Reasoning) ||
            block.text.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += " ";
        }
        out += block.text;
    }
    return out;
}

std::string workspace_relative(const std::string& path, const std::filesystem::path& root) {
    if (path.empty()) {
        return path;
    }
    const std::filesystem::path raw{path};
    if (!raw.is_absolute()) {
        std::string relative = path;
        if (relative.rfind("./", 0) == 0) {
            relative.erase(0, 2);
        }
        return relative;
    }
    const std::filesystem::path lexically = raw.lexically_relative(root);
    if (lexically.empty()) {
        return path;
    }
    const std::string generic = lexically.generic_string();
    if (generic.rfind("..", 0) == 0) {
        return path;
    }
    return generic;
}

bool is_path_key(std::string_view key) {
    return key == "path" || key == "file" || key == "file_path" || key == "filepath" ||
           key == "filePath" || key == "pattern" || key == "glob" || key == "ref";
}

std::string tool_outcome_name(payload::ToolOutcome outcome) {
    switch (outcome) {
        case payload::ToolOutcome::Ok:
            return "ok";
        case payload::ToolOutcome::Error:
            return "error";
        case payload::ToolOutcome::Denied:
            return "denied";
        case payload::ToolOutcome::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

std::string normalize_summary_sections(const std::string& text) {
    std::array<std::string, kHandoffHeadings.size()> bodies{};
    std::size_t     current = kHandoffHeadings.size();
    std::istringstream input{text};
    std::string        line;
    while (std::getline(input, line)) {
        std::string trimmed = line;
        while (!trimmed.empty() &&
               (trimmed.back() == '\r' || std::isspace(static_cast<unsigned char>(trimmed.back())) != 0)) {
            trimmed.pop_back();
        }
        std::size_t index = kHandoffHeadings.size();
        for (std::size_t candidate = 0; candidate < kHandoffHeadings.size(); ++candidate) {
            if (trimmed == kHandoffHeadings[candidate]) {
                index = candidate;
                break;
            }
        }
        if (index != kHandoffHeadings.size()) {
            current = index;
            continue;
        }
        if (current == kHandoffHeadings.size()) {
            continue;
        }
        if (!bodies[current].empty()) {
            bodies[current].push_back('\n');
        }
        bodies[current] += line;
    }

    std::string out;
    for (std::size_t index = 0; index < kHandoffHeadings.size(); ++index) {
        if (index != 0) {
            out.push_back('\n');
        }
        out += kHandoffHeadings[index];
        out.push_back('\n');
        std::string body = bodies[index];
        while (!body.empty() &&
               (body.back() == '\n' || body.back() == '\r' ||
                std::isspace(static_cast<unsigned char>(body.back())) != 0)) {
            body.pop_back();
        }
        out += body.empty() ? std::string{"(none)"} : body;
        out.push_back('\n');
    }
    return out;
}

std::string serialize_view(const std::vector<Message>& view) {
    std::string body;
    for (const Message& message : view) {
        body += role_name(message.role);
        body += ": ";
        for (const ContentBlock& block : message.content) {
            switch (block.kind) {
                case ContentBlockKind::Text:
                    body += block.text;
                    break;
                case ContentBlockKind::Reasoning:
                    body += "[reasoning] ";
                    body += block.text;
                    break;
                case ContentBlockKind::ToolUse:
                    body += "[tool_use ";
                    body += block.tool_name;
                    body += " ";
                    body += block.arguments.dump();
                    body += "]";
                    break;
                case ContentBlockKind::Image:
                    body += "[image]";
                    break;
            }
            body.push_back('\n');
        }
    }
    return body;
}

std::vector<std::size_t> turn_starts(const std::vector<Message>& view) {
    std::vector<std::size_t> starts;
    for (std::size_t index = 0; index < view.size(); ++index) {
        if (view[index].role == Role::User) {
            starts.push_back(index);
        }
    }
    return starts;
}

std::vector<Message> bound_view(const std::vector<Message>& view, std::size_t budget,
                                std::size_t keep_recent_turns) {
    if (serialize_view(view).size() <= budget) {
        return view;
    }
    const std::vector<std::size_t> starts = turn_starts(view);
    const std::size_t              keep   = std::min(keep_recent_turns, starts.size());
    for (std::size_t drop = 1; drop + keep <= starts.size(); ++drop) {
        const std::size_t first_kept = starts[drop];
        std::vector<Message> candidate(view.begin() + static_cast<std::ptrdiff_t>(first_kept),
                                       view.end());
        if (serialize_view(candidate).size() <= budget) {
            return candidate;
        }
    }
    const std::size_t floor_kept = keep == 0 ? view.size() : starts[starts.size() - keep];
    if (floor_kept >= view.size()) {
        return {};
    }
    return std::vector<Message>(view.begin() + static_cast<std::ptrdiff_t>(floor_kept), view.end());
}

std::vector<Message> text_prompt(std::string_view instruction, std::string body) {
    std::vector<Message> prompt;
    Message              system;
    system.role = Role::System;
    ContentBlock instruction_block;
    instruction_block.kind = ContentBlockKind::Text;
    instruction_block.text = std::string{instruction};
    system.content.push_back(std::move(instruction_block));
    prompt.push_back(std::move(system));

    Message user;
    user.role = Role::User;
    ContentBlock user_block;
    user_block.kind = ContentBlockKind::Text;
    user_block.text = std::move(body);
    user.content.push_back(std::move(user_block));
    prompt.push_back(std::move(user));
    return prompt;
}

std::string resolve_summarizer_model(const HandoffPolicy& policy, const Session& session) {
    ModelResolutionSources sources;
    if (!policy.summarizer_model.empty()) {
        sources.request_override = policy.summarizer_model;
    }
    if (!session.header().model.empty()) {
        sources.session_model = session.header().model;
    }
    const std::optional<ModelId> resolved = resolve_model(sources);
    return resolved.value_or(session.header().model);
}

struct SummarizerRoute {
    std::string endpoint;
    std::string profile_id;
    ProviderId  provider;
};

SummarizerRoute summarizer_route(const std::string& model, const Session& session,
                                 const ModelCatalog* catalog) {
    const SessionHeader              header = session.header();
    std::optional<ModelCatalogEntry> durable;
    if (catalog != nullptr) {
        if (header.model_name.has_value() && !header.model_name->empty()) {
            durable = catalog->find(*header.model_name);
        }
        if (!durable.has_value() && !header.model.empty()) {
            durable = catalog->find(header.model);
        }
    }

    SummarizerRoute route;
    const bool      session_model = !model.empty() && model == header.model;
    if (session_model && durable.has_value()) {
        route.endpoint   = durable->endpoint.name;
        route.profile_id = durable->profile.id;
        route.provider   = durable->endpoint.provider;
        return route;
    }
    if (catalog != nullptr && !model.empty()) {
        if (const auto entry = catalog->find(model); entry.has_value()) {
            route.endpoint   = entry->endpoint.name;
            route.profile_id = entry->profile.id;
            route.provider   = entry->endpoint.provider;
            return route;
        }
    }
    if (durable.has_value()) {
        route.endpoint = durable->endpoint.name;
        route.provider = durable->endpoint.provider;
    } else if (catalog != nullptr) {
        route.endpoint = catalog->default_entry().endpoint.name;
        route.provider = catalog->default_entry().endpoint.provider;
    }
    return route;
}

std::string bound_summary(const std::string& summary, const HandoffPolicy& policy) {
    constexpr std::size_t kPerMessageOverhead = 4;
    constexpr std::size_t kBytesPerToken      = 3;
    if (policy.max_summary_tokens <= kPerMessageOverhead) {
        return {};
    }
    const std::size_t limit =
        (policy.max_summary_tokens - kPerMessageOverhead) * kBytesPerToken;
    if (summary.size() <= limit) {
        return summary;
    }
    return summary.substr(0, utf8_prefix(summary, limit));
}

struct SessionSummaryFacts {
    std::size_t    turn_count = 0;
    std::string    last_turn  = "none";
    std::string    goal_first;
    std::string    goal_latest;
    std::string    goal_change;
    std::string    prior_summary;
    std::vector<std::string> decisions;
    std::vector<std::string> errors;
    std::vector<std::string> files;
};

SessionSummaryFacts collect_facts(const SessionHeader& header, const EventRange& events) {
    SessionSummaryFacts facts;
    std::set<std::string> seen_files;
    for (const EventRecord& record : events) {
        const Event& event = record.event;
        switch (event.type) {
            case EventType::TurnStarted:
                ++facts.turn_count;
                facts.last_turn = "interrupted";
                break;
            case EventType::TurnEnded:
                facts.last_turn = "completed";
                break;
            case EventType::TurnCancelled:
                facts.last_turn = "cancelled";
                break;
            case EventType::TurnFailed:
                facts.last_turn = "failed";
                break;
            case EventType::UserMessage: {
                const std::string text =
                    join_text_blocks(event.payload.get<payload::UserMessage>().content);
                if (!text.empty()) {
                    if (facts.goal_first.empty()) {
                        facts.goal_first = text;
                    }
                    facts.goal_latest = text;
                }
                break;
            }
            case EventType::GoalChange: {
                const payload::GoalChange& change = event.payload.get<payload::GoalChange>();
                if (change.goal.has_value()) {
                    facts.goal_change = change.goal->objective;
                }
                break;
            }
            case EventType::AssistantMessage: {
                const std::string text =
                    join_assistant_text(event.payload.get<payload::AssistantMessage>().content);
                if (!text.empty()) {
                    facts.decisions.push_back(text);
                }
                break;
            }
            case EventType::ContextCompaction:
                facts.prior_summary =
                    event.payload.get<payload::ContextCompaction>().summary;
                break;
            case EventType::ToolCall: {
                const payload::ToolCall& call = event.payload.get<payload::ToolCall>();
                if (!call.arguments.is_object()) {
                    break;
                }
                for (auto it = call.arguments.begin(); it != call.arguments.end(); ++it) {
                    if (!it.value().is_string() || !is_path_key(it.key())) {
                        continue;
                    }
                    const std::string relative =
                        workspace_relative(it.value().get<std::string>(), header.cwd);
                    if (relative.empty()) {
                        continue;
                    }
                    if (seen_files.insert(relative).second &&
                        facts.files.size() < kMaxHandoffFiles) {
                        facts.files.push_back(relative);
                    }
                }
                break;
            }
            case EventType::ToolResult: {
                const payload::ToolResult& result = event.payload.get<payload::ToolResult>();
                if (result.outcome == payload::ToolOutcome::Ok) {
                    break;
                }
                const std::string detail =
                    result.error.has_value() && !result.error->empty()
                        ? *result.error
                        : tool_outcome_name(result.outcome);
                facts.errors.push_back(result.name + ": " + detail);
                break;
            }
            default:
                break;
        }
    }
    std::sort(facts.files.begin(), facts.files.end());
    return facts;
}

} // namespace

std::string_view handoff_instruction() noexcept { return kHandoffInstruction; }

std::string build_handoff_evidence(const SessionHeader& header, const EventRange& events) {
    const SessionSummaryFacts facts = collect_facts(header, events);
    std::ostringstream        out;
    out << "[handoff evidence]\n";
    out << "session: " << header.id.value << "\n";
    out << "title: " << header.title << "\n";
    out << "model: " << header.model << "\n";
    out << "kind: " << session_kind_name(header.kind) << "\n";
    out << "parent: " << (header.parentSession.has_value() ? header.parentSession->value
                                                          : std::string{"-"})
        << "\n";
    out << "turn_count: " << facts.turn_count << "\n";
    out << "last_turn: " << facts.last_turn << "\n";
    out << "goal_first: " << facts.goal_first << "\n";
    out << "goal_latest: " << facts.goal_latest << "\n";
    out << "goal_change: " << facts.goal_change << "\n";
    out << "prior_summary: " << facts.prior_summary << "\n";
    out << "decisions:\n";
    for (const std::string& decision : facts.decisions) {
        out << "- " << decision << "\n";
    }
    out << "errors:\n";
    for (const std::string& error : facts.errors) {
        out << "- " << error << "\n";
    }
    out << "files:\n";
    for (const std::string& file : facts.files) {
        out << "- " << file << "\n";
    }
    return out.str();
}

std::filesystem::path resolve_handoff_path(const ExecutionEnvironment& env,
                                           const std::string&          requested,
                                           const SessionId&            session,
                                           std::time_t                 utc_now,
                                           const std::filesystem::path& default_dir) {
    if (!requested.empty()) {
        return env.resolve(requested);
    }
    const std::filesystem::path dir = env.resolve(default_dir.string());
    std::error_code             ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw ToolError{ToolErrorCode::Io, "cannot create " + dir.string()};
    }
    const std::string stem = session.value + "-" + format_stamp(utc_now);
    for (std::size_t suffix = 1;; ++suffix) {
        const std::string name =
            suffix == 1 ? stem + ".md" : stem + "-" + std::to_string(suffix) + ".md";
        const std::filesystem::path candidate = dir / name;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
}

HandoffService::HandoffService(LlmRuntime&           runtime,
                               LLMPool&              pool,
                               SessionManager&       sessions,
                               ExecutionEnvironment& environment,
                               ModelCatalog*         catalog,
                               HandoffPolicy         policy,
                               WallClock             clock)
    : runtime_(runtime),
      pool_(pool),
      sessions_(sessions),
      environment_(environment),
      catalog_(catalog),
      policy_(std::move(policy)),
      clock_(std::move(clock)) {}

HandoffResult HandoffService::run(const Session&              source,
                                  const std::vector<Message>& view,
                                  const HandoffOptions&       options,
                                  CancellationToken           cancel) {
    HandoffResult result;
    if (!policy_.enabled) {
        result.outcome = HandoffResult::Outcome::Disabled;
        result.detail  = "handoff: disabled";
        return result;
    }

    const EventRange events = source.events();
    bool             has_content = false;
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::UserMessage ||
            record.event.type == EventType::AssistantMessage) {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        result.outcome = HandoffResult::Outcome::Empty;
        result.detail  = "handoff: nothing to summarize (empty session)";
        return result;
    }

    const std::string evidence = build_handoff_evidence(source.header(), events);
    constexpr std::string_view kTranscriptMarker = "\n[transcript]\n";
    const std::size_t overhead = evidence.size() + kTranscriptMarker.size();
    const std::size_t digest_budget =
        policy_.max_input_bytes > overhead ? policy_.max_input_bytes - overhead : 0;
    const std::vector<Message> transcript =
        bound_view(view, digest_budget, policy_.keep_recent_turns);
    const std::string serialized = serialize_view(transcript);

    std::string body = evidence;
    body += kTranscriptMarker;
    body += serialized;
    if (body.size() > policy_.max_input_bytes) {
        result.outcome = HandoffResult::Outcome::ContextTooLarge;
        result.detail  = "handoff: context too large";
        return result;
    }

    const std::string model = resolve_summarizer_model(policy_, source);

    LLMRequest request;
    request.model                  = model;
    request.messages               = text_prompt(kHandoffInstruction, std::move(body));
    request.parameters.tool_choice = std::string{"none"};
    request.session_id             = source.id();
    request.purpose                = CallPurpose::Compaction;

    const SummarizerRoute route = summarizer_route(model, source, catalog_);

    LlmCallConfig config;
    config.endpoint    = route.endpoint;
    config.profile_id  = route.profile_id;
    config.provider    = route.provider;
    config.model       = model;
    config.tool_choice = std::string{"none"};

    FrozenRequest frozen = FrozenRequest::freeze(std::move(request), config);

    std::optional<LLMPool::Slot> slot = pool_.acquire(cancel).get();
    if (!slot.has_value()) {
        result.outcome = HandoffResult::Outcome::Cancelled;
        result.detail  = "handoff: cancelled";
        return result;
    }

    std::string summary_text;
    StreamSink  collect = [&](const StreamEvent& event) -> SinkFlow {
        if (const auto* delta = std::get_if<TextDelta>(&event)) {
            summary_text += delta->text;
        }
        return SinkFlow::Continue;
    };

    LLMResponse response;
    try {
        PreparedCall call = runtime_.prepare_call(config, cancel).get();
        response          = call.stream(std::move(frozen), collect, cancel).get();
    } catch (const NoProviderRouteError& error) {
        result.outcome = HandoffResult::Outcome::NoRoute;
        result.detail  = "handoff: no model route";
        (void)error;
        return result;
    } catch (const PreparedCallError&) {
        result.outcome = HandoffResult::Outcome::Internal;
        result.detail  = "handoff: internal error";
        return result;
    }

    if (response.outcome == StreamOutcome::Cancelled || cancel.cancelled()) {
        result.outcome = HandoffResult::Outcome::Cancelled;
        result.detail  = "handoff: cancelled";
        return result;
    }
    if (response.outcome == StreamOutcome::Failed) {
        if (response.error.code == LLMErrorCode::ContextLengthExceeded) {
            result.outcome = HandoffResult::Outcome::ContextTooLarge;
            result.detail  = "handoff: context too large";
            return result;
        }
        result.outcome = HandoffResult::Outcome::NoRoute;
        result.detail  = "handoff: no model route";
        return result;
    }

    std::string summary = bound_summary(summary_text, policy_);
    if (summary.size() > policy_.max_summary_bytes) {
        result.outcome = HandoffResult::Outcome::Internal;
        result.detail  = "handoff: internal error";
        return result;
    }
    summary = to_ascii(normalize_summary_sections(summary));

    const SessionSummaryFacts facts = collect_facts(source.header(), events);

    const std::filesystem::path default_dir = environment_.root() / ".ymh" / "handoffs";
    const std::time_t           now = std::chrono::system_clock::to_time_t(clock_());
    std::filesystem::path       doc_path;
    try {
        doc_path = resolve_handoff_path(environment_, options.out, source.id(), now, default_dir);
    } catch (const ToolError& error) {
        if (error.code() == ToolErrorCode::PathEscape) {
            result.outcome = HandoffResult::Outcome::BadOption;
            result.detail  = std::string{"handoff: "} + error.what();
        } else {
            result.outcome = HandoffResult::Outcome::WriteFailed;
            result.detail  = "handoff: cannot write " + default_dir.string();
        }
        return result;
    } catch (const std::exception& error) {
        result.outcome = HandoffResult::Outcome::BadOption;
        result.detail  = std::string{"handoff: "} + error.what();
        return result;
    }

    std::error_code relative_error;
    const std::filesystem::path shown =
        std::filesystem::relative(doc_path, environment_.root(), relative_error);
    const std::string doc_relative =
        relative_error ? doc_path.string() : shown.generic_string();

    const std::string title = source.header().title.empty() ? std::string{"session"}
                                                            : source.header().title;
    std::ostringstream document;
    document << "<!-- handoff of session " << source.id().value << " (" << title << ") at "
             << format_utc(now) << " -->\n\n";
    document << summary;
    document << "\n## Source\n";
    document << "- session: " << source.id().value << "\n";
    document << "- turns: " << facts.turn_count << "\n";
    document << "- files:";
    if (facts.files.empty()) {
        document << " (none)\n";
    } else {
        document << "\n";
        for (const std::string& file : facts.files) {
            document << "  - " << file << "\n";
        }
    }

    const std::filesystem::path temp_path = doc_path.string() + ".tmp";
    {
        std::ofstream out{temp_path, std::ios::binary | std::ios::trunc};
        if (!out) {
            result.outcome = HandoffResult::Outcome::WriteFailed;
            result.detail  = "handoff: cannot write " + doc_relative;
            return result;
        }
        out << document.str();
        out.flush();
        if (!out) {
            std::error_code cleanup;
            std::filesystem::remove(temp_path, cleanup);
            result.outcome = HandoffResult::Outcome::WriteFailed;
            result.detail  = "handoff: cannot write " + doc_relative;
            return result;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temp_path, doc_path, rename_error);
    if (rename_error) {
        std::error_code cleanup;
        std::filesystem::remove(temp_path, cleanup);
        result.outcome = HandoffResult::Outcome::WriteFailed;
        result.detail  = "handoff: cannot write " + doc_relative;
        return result;
    }

    result.outcome      = HandoffResult::Outcome::Ok;
    result.doc_relative = doc_relative;

    if (!options.seed) {
        result.detail = "handoff: wrote " + doc_relative;
        return result;
    }

    SessionOptions seed_options;
    seed_options.cwd               = source.header().cwd;
    seed_options.serverProfile     = source.header().serverProfile;
    seed_options.model             = source.header().model;
    seed_options.model_name        = source.header().model_name.value_or("");
    seed_options.title             = cap_title(options.title.empty()
                                                   ? "handoff: " + title
                                                   : options.title,
                                               kMaxSessionTitleBytes);
    seed_options.kind              = SessionKind::Root;
    seed_options.depth             = 0;
    seed_options.agent_preset      = source.header().agent_preset;
    seed_options.permission_preset = source.header().permission_preset;
    seed_options.endpoint          = source.header().endpoint;
    seed_options.profile_id        = source.header().profile_id;

    SessionId seed_id;
    try {
        seed_id = sessions_.createSession(seed_options);
        std::shared_ptr<Session> seed = sessions_.sessionPtr(seed_id);
        payload::ContextInjected injected;
        injected.id      = make_event_id().value;
        injected.role    = Role::User;
        injected.text    = summary;
        injected.source  = plugin_message_source("ymh.handoff", ContextFormed(ContextForm::Recall));
        injected.context = ContextFormed(ContextForm::Recall);
        seed->append(injected);
    } catch (const std::exception&) {
        result.outcome = HandoffResult::Outcome::StoreUnavailable;
        result.detail  = "handoff: wrote " + doc_relative + " (seed unavailable)";
        return result;
    }

    result.seed_session = seed_id;
    result.detail       = "handoff: wrote " + doc_relative + " (seed " + seed_id.value + ")";
    return result;
}

} // namespace ymh

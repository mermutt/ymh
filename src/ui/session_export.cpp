#include "ymh/ui/session_export.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "ymh/agent/message.hpp"
#include "ymh/session/events.hpp"

namespace ymh::ui {
namespace {

std::string format_utc(std::chrono::system_clock::time_point point) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(point);
    std::tm           tm{};
    gmtime_r(&seconds, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::string format_utc_ms(std::int64_t epoch_ms) {
    return format_utc(
        std::chrono::system_clock::time_point{std::chrono::milliseconds{epoch_ms}});
}

// Picks a fence long enough that `text` can never terminate it early.
std::string code_fence(std::string_view text) {
    std::size_t longest = 0;
    std::size_t run     = 0;
    for (const char character : text) {
        if (character == '`') {
            ++run;
            longest = std::max(longest, run);
        } else {
            run = 0;
        }
    }
    return std::string(std::max<std::size_t>(3, longest + 1), '`');
}

std::string code_block(std::string_view text, std::string_view language = {}) {
    const std::string fence = code_fence(text);
    std::string       result = fence;
    result += language;
    result += '\n';
    result += text;
    if (result.empty() || result.back() != '\n') {
        result += '\n';
    }
    result += fence;
    result += '\n';
    return result;
}

std::string_view outcome_name(payload::ToolOutcome outcome) {
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

std::string text_of(const std::vector<ContentBlock>& content) {
    std::string result;
    for (const ContentBlock& block : content) {
        if (block.kind != ContentBlockKind::Text) {
            continue;
        }
        if (!result.empty()) {
            result += '\n';
        }
        result += block.text;
    }
    return result;
}

void render_assistant(std::ostringstream& out, const std::vector<ContentBlock>& content) {
    bool wrote = false;
    for (const ContentBlock& block : content) {
        switch (block.kind) {
            case ContentBlockKind::Reasoning:
                out << "<details>\n<summary>Reasoning</summary>\n\n"
                    << block.text << "\n\n</details>\n\n";
                wrote = true;
                break;
            case ContentBlockKind::Text:
                out << block.text << "\n\n";
                wrote = true;
                break;
            case ContentBlockKind::Image:
                out << "_[image";
                if (!block.media_type.empty()) {
                    out << ": " << block.media_type;
                }
                out << "]_\n\n";
                wrote = true;
                break;
            case ContentBlockKind::ToolUse:
                break;
        }
    }
    if (!wrote) {
        out << "_(no content)_\n\n";
    }
}

std::vector<std::string> split_editor(const std::string& command) {
    std::vector<std::string> tokens;
    std::istringstream       stream(command);
    std::string              token;
    while (stream >> token) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

} // namespace

std::string sanitize_export_stem(std::string_view title) {
    std::string stem;
    stem.reserve(std::min<std::size_t>(title.size(), kMaxExportStem));
    bool separator = false;
    for (const char raw : title) {
        const unsigned char character = static_cast<unsigned char>(raw);
        if (std::isalnum(character) != 0 || raw == '-' || raw == '_' || raw == '.') {
            stem.push_back(raw);
            separator = false;
        } else if (std::isspace(character) != 0 || raw == '/' || raw == '\\') {
            if (!stem.empty() && !separator) {
                stem.push_back('-');
                separator = true;
            }
        }
        if (stem.size() >= kMaxExportStem) {
            break;
        }
    }
    while (!stem.empty() && (stem.back() == '-' || stem.back() == '.')) {
        stem.pop_back();
    }
    return stem;
}

std::string export_filename(const std::string& title, std::time_t utc_now) {
    std::string stem = sanitize_export_stem(title);
    if (stem.empty()) {
        stem = "session";
    }
    std::tm tm{};
    gmtime_r(&utc_now, &tm);
    std::ostringstream out;
    out << stem << '-' << std::put_time(&tm, "%Y%m%d-%H%M%S") << ".md";
    return out.str();
}

std::filesystem::path resolve_export_path(const ExecutionEnvironment& env,
                                          const std::string&         requested,
                                          const std::string&         title,
                                          std::time_t                utc_now) {
    const std::string target =
        requested.empty() ? export_filename(title, utc_now) : requested;
    return env.resolve(target);
}

std::string render_session_markdown(const SessionHeader& header, const EventRange& events) {
    std::ostringstream out;
    out << "# " << (header.title.empty() ? std::string{"Session"} : header.title) << "\n\n";
    out << "- **Session**: `" << header.id.value << "`\n";
    out << "- **Created**: " << format_utc_ms(header.createdAt) << " UTC\n";
    if (!header.model.empty()) {
        out << "- **Model**: `" << header.model << "`\n";
    }
    if (!header.serverProfile.empty()) {
        out << "- **Profile**: `" << header.serverProfile << "`\n";
    }
    if (!header.cwd.empty()) {
        out << "- **Workspace**: `" << header.cwd.string() << "`\n";
    }
    out << "\n";

    for (const EventRecord& record : events) {
        const Event&        event = record.event;
        const std::string   stamp = format_utc(event.timestamp);
        switch (event.type) {
            case EventType::TurnStarted: {
                const auto& started = event.payload.get<payload::TurnStarted>();
                out << "## Turn " << started.turn << "\n\n";
                break;
            }
            case EventType::UserMessage: {
                const auto& message = event.payload.get<payload::UserMessage>();
                out << "### User — " << stamp << " UTC\n\n";
                const std::string text = text_of(message.content);
                out << (text.empty() ? std::string{"_(empty)_"} : text) << "\n\n";
                break;
            }
            case EventType::AssistantMessage: {
                const auto& message = event.payload.get<payload::AssistantMessage>();
                out << "### Assistant — " << stamp << " UTC\n\n";
                render_assistant(out, message.content);
                break;
            }
            case EventType::ToolCall: {
                const auto& call = event.payload.get<payload::ToolCall>();
                out << "### Tool call: `" << call.name << "` — " << stamp << " UTC\n\n";
                out << "<details>\n<summary>Arguments</summary>\n\n";
                out << code_block(call.arguments.dump(2), "json");
                out << "\n</details>\n\n";
                break;
            }
            case EventType::ToolResult: {
                const auto& result = event.payload.get<payload::ToolResult>();
                out << "<details>\n<summary>Output (" << outcome_name(result.outcome);
                if (result.truncated) {
                    out << ", truncated";
                }
                out << ")</summary>\n\n";
                out << code_block(result.output);
                if (result.error.has_value() && !result.error->empty()) {
                    out << "\n_Error: " << *result.error << "_\n";
                }
                out << "\n</details>\n\n";
                break;
            }
            case EventType::TurnCancelled: {
                const auto& cancelled = event.payload.get<payload::TurnCancelled>();
                out << "_Turn cancelled";
                if (!cancelled.reason.empty()) {
                    out << ": " << cancelled.reason;
                }
                out << "_\n\n";
                break;
            }
            case EventType::TurnFailed: {
                const auto& failed = event.payload.get<payload::TurnFailed>();
                out << "_Turn failed (" << failed.code << ")";
                if (!failed.message.empty()) {
                    out << ": " << failed.message;
                }
                out << "_\n\n";
                break;
            }
            case EventType::ContextInjected: {
                const auto& injected = event.payload.get<payload::ContextInjected>();
                out << "### Context (injected) — " << stamp << " UTC\n\n"
                    << injected.text << "\n\n";
                break;
            }
            case EventType::ContextCompaction: {
                const auto& compaction = event.payload.get<payload::ContextCompaction>();
                out << "<details>\n<summary>Context compaction — "
                    << format_utc(compaction.createdAt) << " UTC</summary>\n\n"
                    << compaction.summary << "\n\n</details>\n\n";
                break;
            }
            case EventType::AssistantAttempt:
            case EventType::GoalChange:
            case EventType::CommandRun:
            case EventType::CommandDone:
            case EventType::JobChanged:
                break;
            default:
                break;
        }
    }
    return out.str();
}

bool write_export_file(const std::filesystem::path& path, std::string_view markdown) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(markdown.data(), static_cast<std::streamsize>(markdown.size()));
    return static_cast<bool>(out);
}

std::string editor_from_environment() {
    const char* visual = std::getenv("VISUAL");
    if (visual != nullptr && *visual != '\0') {
        return visual;
    }
    const char* editor = std::getenv("EDITOR");
    if (editor != nullptr && *editor != '\0') {
        return editor;
    }
    return "vi";
}

int run_editor(const std::filesystem::path& file, const std::string& editor) {
    std::vector<std::string> argv = split_editor(editor);
    if (argv.empty()) {
        return -1;
    }
    argv.push_back(file.string());

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (std::string& argument : argv) {
        raw.push_back(argument.data());
    }
    raw.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        ::execvp(raw.front(), raw.data());
        ::_exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

} // namespace ymh::ui

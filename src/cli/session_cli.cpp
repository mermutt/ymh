#include "ymh/cli/session_cli.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"

namespace ymh {
namespace {

std::string format_time(std::int64_t epoch_ms) {
    const std::time_t seconds = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm           tm{};
    gmtime_r(&seconds, &tm);
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

std::string_view kind_name(SessionKind kind) {
    switch (kind) {
        case SessionKind::Root:
            return "root";
        case SessionKind::Fork:
            return "fork";
        case SessionKind::Subagent:
            return "subagent";
    }
    return "unknown";
}

std::filesystem::path database_path(const std::filesystem::path& workspace) {
    return workspace / ".ymh" / "sessions.db";
}

std::filesystem::path lock_path(const std::filesystem::path& workspace) {
    return workspace / ".ymh" / "sessions.lock";
}

PersistenceConfig persistence_config(const std::filesystem::path& workspace) {
    PersistenceConfig config;
    config.db_path   = database_path(workspace);
    config.lock_path = lock_path(workspace);
    config.boot_id   = BootId{make_boot_id()};
    return config;
}

std::string first_text(const std::vector<ContentBlock>& content) {
    for (const ContentBlock& block : content) {
        if (block.kind == ContentBlockKind::Text) {
            return block.text;
        }
    }
    return {};
}

std::string truncate(std::string text, std::size_t max_bytes) {
    const std::size_t newline = text.find('\n');
    if (newline != std::string::npos) {
        text.resize(newline);
    }
    if (text.size() > max_bytes) {
        text.resize(max_bytes);
        text += "...";
    }
    return text;
}

std::string event_detail(const Event& event) {
    switch (event.type) {
        case EventType::SessionStarted: {
            const auto& started = event.payload.get<payload::SessionStarted>();
            return "model=" + started.model + " profile=" + started.serverProfile;
        }
        case EventType::UserMessage: {
            const auto& message = event.payload.get<payload::UserMessage>();
            return truncate(first_text(message.content), 80);
        }
        case EventType::AssistantChunk: {
            const auto& chunk = event.payload.get<payload::AssistantChunk>();
            const std::string_view kind =
                chunk.kind == payload::AssistantChunkKind::Text ? "text" : "reasoning";
            return std::string{kind} + " " + truncate(chunk.text, 80);
        }
        case EventType::AssistantMessage: {
            const auto& assistant = event.payload.get<payload::AssistantMessage>();
            return truncate(first_text(assistant.content), 80);
        }
        case EventType::ToolCall: {
            const auto& call = event.payload.get<payload::ToolCall>();
            return call.name + " " + truncate(call.arguments.dump(), 80);
        }
        case EventType::ToolResult: {
            const auto& result = event.payload.get<payload::ToolResult>();
            std::string_view outcome = "ok";
            switch (result.outcome) {
                case payload::ToolOutcome::Ok:
                    outcome = "ok";
                    break;
                case payload::ToolOutcome::Error:
                    outcome = "error";
                    break;
                case payload::ToolOutcome::Denied:
                    outcome = "denied";
                    break;
                case payload::ToolOutcome::Cancelled:
                    outcome = "cancelled";
                    break;
            }
            return result.name + " " + std::string{outcome} + " " + truncate(result.output, 80);
        }
        case EventType::TurnFailed: {
            const auto& failed = event.payload.get<payload::TurnFailed>();
            return failed.code + " " + truncate(failed.message, 80);
        }
        case EventType::TurnCancelled: {
            const auto& cancelled = event.payload.get<payload::TurnCancelled>();
            return "reason=" + cancelled.reason;
        }
        default:
            return {};
    }
}

void print_message(std::ostream& out, const Message& message) {
    out << '[' << role_name(message.role) << "] ";
    bool first = true;
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Text) {
            if (!first) {
                out << '\n';
            }
            out << block.text;
            first = false;
        } else if (block.kind == ContentBlockKind::ToolUse) {
            if (!first) {
                out << '\n';
            }
            out << "[tool_use " << block.tool_name << ']';
            first = false;
        }
    }
    out << '\n';
}

int require_store(const std::filesystem::path& workspace, std::ostream& err) {
    std::error_code error;
    if (!std::filesystem::exists(database_path(workspace), error)) {
        err << "ymh: no session store in " << workspace << " (run `ymh run \"task\"` first)\n";
        return 2;
    }
    return 0;
}

} // namespace

int session_list(const std::filesystem::path& workspace, std::ostream& out, std::ostream& err) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::canonical(workspace, error);
    if (error) {
        err << "ymh: workspace not found: " << workspace << '\n';
        return 2;
    }
    std::error_code exists_error;
    if (!std::filesystem::exists(database_path(root), exists_error)) {
        out << "no sessions\n";
        return 0;
    }

    std::unique_ptr<SessionPersistence> store;
    try {
        store = SessionPersistence::openReadOnly(persistence_config(root));
    } catch (const std::exception& open_error) {
        err << "ymh: cannot open session store: " << open_error.what() << '\n';
        return 2;
    }

    std::vector<SessionHeader> headers = store->list();
    std::sort(headers.begin(), headers.end(), [](const SessionHeader& left, const SessionHeader& right) {
        return left.updatedAt > right.updatedAt;
    });

    if (headers.empty()) {
        out << "no sessions\n";
        return 0;
    }
    out << std::left << std::setw(38) << "ID" << std::setw(21) << "UPDATED" << std::setw(9) << "KIND"
        << "MODEL\n";
    for (const SessionHeader& header : headers) {
        out << std::left << std::setw(38) << header.id.value << std::setw(21)
            << format_time(header.updatedAt) << std::setw(9) << kind_name(header.kind) << header.model;
        if (!header.title.empty()) {
            out << "  " << truncate(header.title, 60);
        }
        out << '\n';
    }
    return 0;
}

int session_show(const std::filesystem::path& workspace,
                 const std::string&         session,
                 std::ostream&              out,
                 std::ostream&              err) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::canonical(workspace, error);
    if (error) {
        err << "ymh: workspace not found: " << workspace << '\n';
        return 2;
    }
    if (const int status = require_store(root, err); status != 0) {
        return status;
    }

    std::unique_ptr<SessionPersistence> store;
    try {
        store = SessionPersistence::openReadOnly(persistence_config(root));
    } catch (const std::exception& open_error) {
        err << "ymh: cannot open session store: " << open_error.what() << '\n';
        return 2;
    }

    const std::optional<SessionHeader> header = store->load(SessionId{session});
    if (!header.has_value()) {
        err << "ymh: unknown session: " << session << '\n';
        return 2;
    }

    const EventRange events = store->read(header->id);
    const std::vector<Message> messages = deriveMessages(*header, events);

    out << "session  " << header->id.value << '\n';
    out << "workspace " << header->cwd.string() << '\n';
    out << "created  " << format_time(header->createdAt) << '\n';
    out << "updated  " << format_time(header->updatedAt) << '\n';
    out << "kind     " << kind_name(header->kind) << '\n';
    out << "model    " << header->model << '\n';
    out << "profile  " << header->serverProfile << '\n';
    if (!header->title.empty()) {
        out << "title    " << header->title << '\n';
    }
    out << "events   " << events.size() << '\n';
    out << "---\n";
    for (const Message& message : messages) {
        print_message(out, message);
    }
    return 0;
}

int session_replay(const std::filesystem::path& workspace,
                   const std::string&         session,
                   std::ostream&              out,
                   std::ostream&              err) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::canonical(workspace, error);
    if (error) {
        err << "ymh: workspace not found: " << workspace << '\n';
        return 2;
    }
    if (const int status = require_store(root, err); status != 0) {
        return status;
    }

    std::unique_ptr<SessionPersistence> store;
    try {
        store = SessionPersistence::openReadOnly(persistence_config(root));
    } catch (const std::exception& open_error) {
        err << "ymh: cannot open session store: " << open_error.what() << '\n';
        return 2;
    }

    const std::optional<SessionHeader> header = store->load(SessionId{session});
    if (!header.has_value()) {
        err << "ymh: unknown session: " << session << '\n';
        return 2;
    }

    const EventRange events = store->read(header->id);
    for (const EventRecord& record : events) {
        out << std::setw(5) << std::setfill('0') << record.seq << std::setfill(' ') << "  "
            << wire_name(record.event.type);
        const std::string detail = event_detail(record.event);
        if (!detail.empty()) {
            out << "  " << detail;
        }
        out << '\n';
    }
    return 0;
}

int session_fork(const std::filesystem::path& workspace,
                 const std::string&         session,
                 std::ostream&              out,
                 std::ostream&              err) {
    std::error_code error;
    const std::filesystem::path root = std::filesystem::canonical(workspace, error);
    if (error) {
        err << "ymh: workspace not found: " << workspace << '\n';
        return 2;
    }
    if (const int status = require_store(root, err); status != 0) {
        return status;
    }

    std::unique_ptr<SessionPersistence> store;
    try {
        store = SessionPersistence::open(persistence_config(root));
    } catch (const std::exception& open_error) {
        err << "ymh: cannot open session store: " << open_error.what() << '\n';
        return 2;
    }

    const std::optional<SessionHeader> header = store->load(SessionId{session});
    if (!header.has_value()) {
        err << "ymh: unknown session: " << session << '\n';
        return 2;
    }

    EventBus       bus;
    SessionManager sessions(*store, bus);
    try {
        const std::size_t seed_length = store->read(header->id).size();
        const SessionId   child       = sessions.forkSession(header->id, seed_length);
        out << child.value << '\n';
        store->releaseLease(child);
    } catch (const std::exception& fork_error) {
        err << "ymh: cannot fork session: " << fork_error.what() << '\n';
        return 2;
    }
    return 0;
}

} // namespace ymh

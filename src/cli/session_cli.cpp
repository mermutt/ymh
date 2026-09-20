#include "ymh/cli/session_cli.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/transport/host.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"

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
    config.boot_id   = mint_boot_id();
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
        case EventType::AssistantAttempt:
        case EventType::GoalChange:
            return {};
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

    std::vector<SessionHeader> headers;
    for (const SessionHeader& header : store->list()) {
        // 23 §6.1: `ymh list` hides unprompted roots; `show`/`replay`/`fork` keep
        // resolving them by explicit id.
        if (header.kind == SessionKind::Root && store->isUnprompted(header.id)) {
            continue;
        }
        headers.push_back(header);
    }
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

namespace {

struct PruneEntry {
    WorkspaceId  workspace;
    SessionId    id;
    SessionKind  kind      = SessionKind::Root;
    std::int64_t updatedAt = 0;
    std::string  status;
    std::string  reason;
    bool         failed = false;
};

std::vector<SessionHeader> select_empty_roots(const SessionStore& store,
                                              const std::optional<std::size_t>& keep) {
    std::vector<SessionHeader> selected;
    for (const SessionHeader& header : store.list()) {
        if (header.kind == SessionKind::Root && store.isUnprompted(header.id)) {
            selected.push_back(header);
        }
    }
    std::sort(selected.begin(), selected.end(),
              [](const SessionHeader& left, const SessionHeader& right) {
                  if (left.updatedAt != right.updatedAt) {
                      return left.updatedAt > right.updatedAt;
                  }
                  return left.id.value < right.id.value;
              });
    if (keep.has_value() && *keep < selected.size()) {
        selected.erase(selected.begin(), selected.begin() + static_cast<std::ptrdiff_t>(*keep));
    }
    return selected;
}

nlohmann::json prune_json(const std::vector<PruneEntry>& entries) {
    nlohmann::json array = nlohmann::json::array();
    for (const PruneEntry& entry : entries) {
        array.push_back(nlohmann::json{{"id", entry.id.value},
                                       {"kind", std::string{kind_name(entry.kind)}},
                                       {"updated_at", entry.updatedAt},
                                       {"status", entry.status},
                                       {"reason", entry.reason}});
    }
    return array;
}

void mark_all_failed(std::vector<PruneEntry*>& entries, const std::string& reason) {
    for (PruneEntry* entry : entries) {
        entry->status = "failed";
        entry->reason = reason;
        entry->failed = true;
    }
}

bool apply_live(const WorkspaceRecord& record, std::vector<PruneEntry*>& entries, bool force,
                std::ostream& err) {
    try {
        protocol::HostConnection connection;
        connection.connect(record.host->socketPath.string());
        [[maybe_unused]] const protocol::HelloResult hello = connection.handshake(
            protocol::ServerProfile::Interactive,
            protocol::ClientInstanceId{generate_uuid_v4()}, protocol::ClientRole::Observer);
        for (PruneEntry* entry : entries) {
            try {
                [[maybe_unused]] const nlohmann::json reply =
                    connection.request(protocol::method::kSessionDelete,
                                       {{"session", entry->id.value},
                                        {"confirm", true},
                                        {"only_if_empty", true},
                                        {"force", force}});
                entry->status = "pruned";
                entry->reason.clear();
                entry->failed = false;
            } catch (const protocol::RpcException& rpc_error) {
                entry->failed = false;
                if (rpc_error.code() == static_cast<int>(protocol::AppCode::DependentSession)) {
                    entry->status = "skipped";
                    entry->reason = "has dependent sessions";
                } else if (rpc_error.code() == static_cast<int>(protocol::RpcCode::InvalidParams)) {
                    entry->status = "skipped";
                    entry->reason = rpc_error.what();
                } else if (rpc_error.code() == static_cast<int>(protocol::AppCode::UnknownSession)) {
                    entry->status = "already gone";
                    entry->reason = "already gone";
                } else {
                    entry->status = "failed";
                    entry->reason = rpc_error.what();
                    entry->failed = true;
                }
            }
        }
        connection.close();
        return true;
    } catch (const std::exception& connect_error) {
        err << "ymh: cannot delete via daemon " << record.id.value << ": " << connect_error.what()
            << '\n';
        mark_all_failed(entries, connect_error.what());
        return false;
    }
}

bool apply_stopped(const WorkspaceRecord& record, std::vector<PruneEntry*>& entries,
                   std::unique_ptr<WorkspaceRegistry>& writer, std::ostream& err) {
    const std::filesystem::path db_path = record.canonicalPath / ".ymh" / "sessions.db";
    std::error_code             exists_error;
    const bool db_preexisted = std::filesystem::exists(db_path, exists_error) && !exists_error;
    if (!db_preexisted) {
        mark_all_failed(entries, "no sessions.db");
        return false;
    }

    std::unique_ptr<SessionPersistence> store;
    try {
        store = SessionPersistence::open(persistence_config(record.canonicalPath));
    } catch (const std::exception& open_error) {
        err << "ymh: cannot open session store for " << record.canonicalPath << ": "
            << open_error.what() << '\n';
        mark_all_failed(entries, open_error.what());
        return false;
    }

    if (writer == nullptr) {
        writer = WorkspaceRegistry::open(default_registry_config());
    }

    EventBus       bus;
    SessionManager manager(*store, bus);
    for (PruneEntry* entry : entries) {
        try {
            entry->failed = false;
            if (store->hasDependents(entry->id)) {
                entry->status = "skipped";
                entry->reason = "has dependent sessions";
                continue;
            }
            writer->removeSession(record.id, entry->id);
            try {
                manager.deleteSession(entry->id, true);
                entry->status = "pruned";
                entry->reason.clear();
            } catch (const std::invalid_argument&) {
                entry->status = "skipped";
                entry->reason = "not empty";
            } catch (const DependentSessionError&) {
                entry->status = "skipped";
                entry->reason = "has dependent sessions";
            } catch (const UnknownSession&) {
                entry->status = "already gone";
                entry->reason = "already gone";
            }
        } catch (const std::exception& delete_error) {
            entry->status = "failed";
            entry->reason = delete_error.what();
            entry->failed = true;
        }
    }

    // 23 §7: the orphan-junction sweep runs only in this flock-held stopped-path
    // apply, and only when sessions.db pre-existed the writable open.
    for (const WorkspaceSessionRecord& junction : writer->listSessions(record.id)) {
        if (!store->load(junction.sessionId).has_value()) {
            try {
                writer->removeSession(record.id, junction.sessionId);
            } catch (const std::exception& sweep_error) {
                err << "ymh: sweep failed for " << junction.sessionId.value << ": "
                    << sweep_error.what() << '\n';
            }
        }
    }
    return true;
}

} // namespace

int session_prune(const PruneOptions& options, std::ostream& out, std::ostream& err) {
    if (!options.empty) {
        err << "session prune: pass --empty (--keep is a modifier only)\n";
        return 2;
    }
    if (options.all && options.workspace.has_value()) {
        err << "session prune: --workspace and --all are mutually exclusive\n";
        return 2;
    }

    std::unique_ptr<WorkspaceRegistry> registry;
    try {
        registry = WorkspaceRegistry::openReadOnly(default_registry_config());
    } catch (const std::exception& registry_error) {
        err << "ymh: registry unavailable: " << registry_error.what() << '\n';
        return 1;
    }

    std::vector<WorkspaceRecord> targets;
    if (options.all) {
        targets = registry->listWorkspaces();
    } else {
        const std::filesystem::path requested =
            options.workspace.value_or(std::filesystem::current_path());
        std::error_code             error;
        const std::filesystem::path canonical = std::filesystem::canonical(requested, error);
        if (error) {
            err << "ymh: workspace not found: " << requested << '\n';
            return 2;
        }
        const std::optional<WorkspaceRecord> record = registry->findByCanonicalPath(canonical);
        if (!record.has_value()) {
            err << "ymh: unregistered workspace: " << canonical << '\n';
            return 2;
        }
        targets.push_back(*record);
    }

    std::vector<PruneEntry> entries;
    for (const WorkspaceRecord& record : targets) {
        const std::filesystem::path db_path = record.canonicalPath / ".ymh" / "sessions.db";
        std::error_code             error;
        if (!std::filesystem::exists(db_path, error) || error) {
            continue;
        }
        std::unique_ptr<SessionPersistence> store;
        try {
            store = SessionPersistence::openReadOnly(persistence_config(record.canonicalPath));
        } catch (const std::exception& open_error) {
            err << "ymh: cannot read session store for " << record.canonicalPath << ": "
                << open_error.what() << '\n';
            continue;
        }
        for (const SessionHeader& header : select_empty_roots(*store, options.keep)) {
            PruneEntry entry;
            entry.workspace = record.id;
            entry.id        = header.id;
            entry.kind      = header.kind;
            entry.updatedAt = header.updatedAt;
            entry.status    = options.yes ? "pending" : "would prune";
            entry.reason    = "empty root";
            entries.push_back(std::move(entry));
        }
    }

    if (!options.yes) {
        if (options.json) {
            out << prune_json(entries).dump(2) << '\n';
        } else {
            out << "would prune " << entries.size() << '\n';
            for (const PruneEntry& entry : entries) {
                out << entry.id.value << '\t' << kind_name(entry.kind) << '\t' << entry.updatedAt
                    << '\t' << entry.reason << '\n';
            }
        }
        return 0;
    }

    std::unique_ptr<WorkspaceRegistry> writer;
    bool                               any_failed = false;
    for (const WorkspaceRecord& record : targets) {
        std::vector<PruneEntry*> mine;
        for (PruneEntry& entry : entries) {
            if (entry.workspace == record.id) {
                mine.push_back(&entry);
            }
        }
        const bool live = record.host.has_value() &&
                          registry->probeLiveness(record.id) == HostLiveness::Live;
        bool path_ok = true;
        if (live) {
            if (!mine.empty()) {
                path_ok = apply_live(record, mine, options.force, err);
            }
        } else {
            try {
                path_ok = apply_stopped(record, mine, writer, err);
            } catch (const std::exception& apply_error) {
                err << "ymh: prune failed for " << record.id.value << ": " << apply_error.what()
                    << '\n';
                mark_all_failed(mine, apply_error.what());
                path_ok = false;
            }
        }
        if (path_ok) {
            continue;
        }
        // SL-F8: the chosen path could not be taken (RPC connect or flock open);
        // re-classify liveness and retry the other path once.
        const std::optional<WorkspaceRecord> fresh =
            registry->findByCanonicalPath(record.canonicalPath);
        const bool retry_live = fresh.has_value() && fresh->host.has_value() &&
                                registry->probeLiveness(fresh->id) == HostLiveness::Live;
        const WorkspaceRecord& target = fresh.has_value() ? *fresh : record;
        if (retry_live && !live) {
            if (!mine.empty()) {
                apply_live(target, mine, options.force, err);
            }
        } else if (!retry_live && live) {
            try {
                apply_stopped(target, mine, writer, err);
            } catch (const std::exception& apply_error) {
                err << "ymh: prune failed for " << record.id.value << ": " << apply_error.what()
                    << '\n';
                mark_all_failed(mine, apply_error.what());
            }
        }
    }

    std::size_t pruned  = 0;
    std::size_t skipped = 0;
    for (const PruneEntry& entry : entries) {
        if (entry.failed) {
            any_failed = true;
        }
        if (entry.status == "pruned") {
            ++pruned;
        } else if (entry.status == "skipped" || entry.status == "already gone") {
            ++skipped;
        }
    }

    if (options.json) {
        out << prune_json(entries).dump(2) << '\n';
    } else {
        out << "pruned " << pruned << " skipped " << skipped << '\n';
        for (const PruneEntry& entry : entries) {
            if (entry.status != "pruned") {
                out << entry.id.value << '\t' << entry.status << '\t' << entry.reason << '\n';
            }
        }
    }
    return any_failed ? 1 : 0;
}

} // namespace ymh

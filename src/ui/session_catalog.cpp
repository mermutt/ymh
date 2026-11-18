#include "ymh/ui/session_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

#include "ymh/session/errors.hpp"
#include "ymh/session/session_persistence.hpp"

namespace ymh::ui {
namespace {

std::filesystem::path db_path_of(const std::filesystem::path& workspace) {
    return workspace / ".ymh" / "sessions.db";
}

std::filesystem::path store_dir_of(const std::filesystem::path& workspace) {
    return workspace / ".ymh" / "sessions";
}

std::filesystem::path ymh_dir_of(const std::filesystem::path& workspace) {
    return workspace / ".ymh";
}

bool has_legacy_store(const std::filesystem::path& workspace) {
    std::error_code error;
    return std::filesystem::is_regular_file(db_path_of(workspace), error) && !error;
}

bool has_file_store(const std::filesystem::path& workspace) {
    std::error_code error;
    return std::filesystem::is_directory(store_dir_of(workspace), error) && !error;
}

// §4.4 "read-only location" nuance: the file store only needs read access to
// the directory tree, and one-time migration needs write access.
bool is_read_only_location(const std::filesystem::path& ymh_dir) {
    return ::access(ymh_dir.c_str(), W_OK) != 0;
}

SessionHistoryEntry to_entry(const SessionHeader& header) {
    SessionHistoryEntry entry;
    entry.id         = header.id;
    entry.title      = header.title;
    entry.kind       = std::string{session_kind_name(header.kind)};
    entry.model      = header.model;
    entry.createdAt  = header.createdAt;
    entry.updatedAt  = header.updatedAt;
    entry.parent     = header.parentSession;
    entry.seedLength = header.seedLength;
    return entry;
}

void sort_sessions(std::vector<SessionHistoryEntry>& sessions) {
    std::sort(sessions.begin(), sessions.end(), [](const auto& left, const auto& right) {
        if (left.updatedAt != right.updatedAt) {
            return left.updatedAt > right.updatedAt;
        }
        return left.id.value < right.id.value;
    });
}

WorkspaceHistory note_history(const WorkspaceRecord& record, bool live, std::string note) {
    WorkspaceHistory history;
    history.id            = record.id;
    history.title         = record.displayTitle;
    history.canonicalPath = record.canonicalPath.string();
    history.live          = live;
    history.note          = std::move(note);
    return history;
}

} // namespace

WorkspaceHistory read_workspace_history(const WorkspaceRecord& record, bool live,
                                        bool include_unprompted) {
    std::error_code error;

    // §4.4 pre-flight, in pinned order. Step 0 runs before the DB existence
    // check so a deleted workspace classifies as "workspace missing" and the
    // `Impl::create` workspace-root branch stays TOCTOU-only.
    if (!std::filesystem::is_directory(record.canonicalPath, error)) {
        return note_history(record, live, "workspace missing");
    }
    const std::filesystem::path ymh_dir = ymh_dir_of(record.canonicalPath);
    const std::filesystem::path db_path = db_path_of(record.canonicalPath);
    if (!has_file_store(record.canonicalPath) && !has_legacy_store(record.canonicalPath)) {
        return note_history(record, live, "no sessions");
    }
    if (has_file_store(record.canonicalPath) &&
        !std::filesystem::is_directory(store_dir_of(record.canonicalPath), error)) {
        return note_history(record, live, "not a directory");
    }
    if (!has_file_store(record.canonicalPath) &&
        !std::filesystem::is_regular_file(db_path, error)) {
        return note_history(record, live, "not a file");
    }
    // Permission-dependent (LOW-5): `access` is bypassed by root, so this and
    // the "read-only location" arm are pinned under the non-root assumption.
    const std::filesystem::path readable =
        has_file_store(record.canonicalPath) ? store_dir_of(record.canonicalPath) : db_path;
    if (::access(readable.c_str(), R_OK) != 0) {
        return note_history(record, live, "unreadable");
    }

    WorkspaceHistory history;
    history.id            = record.id;
    history.title         = record.displayTitle;
    history.canonicalPath = record.canonicalPath.string();
    history.live          = live;

    // §4.4 catch order (pinned): specific `StoreError` subclasses first.
    try {
        PersistenceConfig config;
        config.db_path   = db_path;
        config.lock_path = ymh_dir / "sessions.lock";
        config.boot_id   = BootId{"catalog-read"};
        const std::unique_ptr<SessionPersistence> store = SessionPersistence::openReadOnly(config);
        for (const SessionHeader& header : store->list()) {
            // 23 §6.1/§6.2: the filter is applied here only when the caller (the
            // `/sessions` catalog consumer) asks for it; the default keeps the
            // `--resume` resolver able to find a hidden unprompted id.
            if (!include_unprompted && header.kind == SessionKind::Root &&
                store->isUnprompted(header.id)) {
                continue;
            }
            history.sessions.push_back(to_entry(header));
        }
        sort_sessions(history.sessions);
    } catch (const SchemaVersionError&) {
        history.note = "schema mismatch";
    } catch (const CorruptionError&) {
        history.note = "corrupt";
    } catch (const StoreOpenError& error) {
        history.note = "unavailable: " + std::string{error.what()};
    } catch (const StoreError& error) {
        history.note = is_read_only_location(ymh_dir) ? "read-only location"
                                                      : "unavailable: " + std::string{error.what()};
    } catch (const std::exception& error) {
        history.note = "unavailable: " + std::string{error.what()};
    }
    // 57-D6: after the try/catch so a mid-loop throw leaves `lastUsedAt == 0`
    // (never a partial value); `note_history` keeps the default 0.
    if (!history.note.has_value()) {
        std::int64_t last_used = 0;
        for (const SessionHistoryEntry& entry : history.sessions) {
            last_used = std::max(last_used, entry.updatedAt);
        }
        history.lastUsedAt = last_used;
    }
    return history;
}

WorkspaceCatalogSource registry_catalog_source(WorkspaceRegistry& registry) {
    WorkspaceCatalogSource source;
    source.list = [&registry] { return registry.listWorkspaces(); };
    source.is_live = [&registry](const WorkspaceId& id) {
        return registry.probeLiveness(id) == HostLiveness::Live;
    };
    source.read = [](const WorkspaceRecord& record, bool live) {
        // 23 §6.1/23-D31: the `/sessions` catalog consumer hides unprompted root
        // headers; every other caller keeps the unfiltered default.
        return read_workspace_history(record, live, false);
    };
    return source;
}

SessionCatalogReader::SessionCatalogReader(WorkspaceRegistry& registry, Sink sink,
                                           std::chrono::milliseconds refresh_interval)
    : SessionCatalogReader(registry_catalog_source(registry), std::move(sink), refresh_interval) {}

SessionCatalogReader::SessionCatalogReader(WorkspaceCatalogSource source, Sink sink,
                                           std::chrono::milliseconds refresh_interval)
    : source_(std::move(source)), sink_(std::move(sink)), refresh_interval_(refresh_interval) {}

SessionCatalogReader::~SessionCatalogReader() { stop(); }

void SessionCatalogReader::start() {
    {
        const std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
    }
    thread_ = std::thread([this] { loop(); });
}

void SessionCatalogReader::stop() {
    {
        const std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SessionCatalogReader::refreshNow() {
    {
        const std::lock_guard lock(mutex_);
        refresh_pending_ = true;
    }
    cv_.notify_all();
}

SessionCatalogSnapshot SessionCatalogReader::build() const {
    SessionCatalogSnapshot snapshot;
    const std::vector<WorkspaceRecord> records = source_.list();
    snapshot.workspaces.reserve(records.size());
    bool complete = true;
    for (const WorkspaceRecord& record : records) {
        const bool live = source_.is_live(record.id);
        WorkspaceHistory history = source_.read(record, live);
        if (history.note.has_value()) {
            complete = false;
        }
        snapshot.workspaces.push_back(std::move(history));
    }
    snapshot.complete = complete;
    return snapshot;
}

void SessionCatalogReader::loop() {
    std::unique_lock lock(mutex_);
    while (running_) {
        cv_.wait_for(lock, refresh_interval_, [this] { return !running_ || refresh_pending_; });
        if (!running_) {
            return;
        }
        refresh_pending_ = false;
        lock.unlock();
        try {
            SessionCatalogSnapshot snapshot = build();
            snapshot.generation = ++generation_;
            if (sink_) {
                sink_(std::move(snapshot));
            }
        } catch (const std::exception&) {
            // §4.4 "Registry read fails" / "Reader thread exception": keep the
            // last snapshot and retry on the next tick.
        } catch (...) {
        }
        lock.lock();
    }
}

} // namespace ymh::ui

#include "ymh/session/session_persistence.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>

#include <sqlite3.h>

#include "ymh/session/errors.hpp"
#include "ymh/session/session.hpp"

namespace ymh {
namespace {

constexpr char kDdl[] = R"sql(
CREATE TABLE sessions (
    id              TEXT PRIMARY KEY,
    cwd             TEXT NOT NULL,
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL,
    title           TEXT NOT NULL DEFAULT '',
    model           TEXT NOT NULL DEFAULT '',
    server_profile  TEXT NOT NULL DEFAULT 'interactive',
    kind            TEXT NOT NULL DEFAULT 'root',
    parent_session  TEXT,
    seed_length     INTEGER,
    metadata        JSON,
    FOREIGN KEY(parent_session) REFERENCES sessions(id),
    CHECK (kind IN ('root','fork','subagent')),
    CHECK ((kind='root' AND parent_session IS NULL AND seed_length IS NULL) OR (kind='fork' AND parent_session IS NOT NULL AND seed_length IS NOT NULL) OR (kind='subagent' AND parent_session IS NOT NULL AND (seed_length IS NULL OR seed_length = 0)))
);

CREATE TABLE events (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id TEXT NOT NULL,
    event_id TEXT NOT NULL UNIQUE,
    timestamp INTEGER NOT NULL,
    type TEXT NOT NULL,
    payload JSON NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(id)
);

CREATE INDEX idx_events_session
ON events(session_id, sequence);

CREATE TABLE session_leases (
    session_id     TEXT PRIMARY KEY,
    holder_pid     INTEGER NOT NULL,
    holder_boot_id TEXT NOT NULL,
    acquired_at    INTEGER NOT NULL,
    expires_at     INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(id)
);

CREATE TABLE session_snapshots (
    session_id     TEXT PRIMARY KEY,
    at_sequence    INTEGER NOT NULL,
    header_json    TEXT NOT NULL,
    messages_json  TEXT NOT NULL,
    event_count    INTEGER NOT NULL,
    created_at     INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
)sql";

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t to_epoch_ms(std::chrono::system_clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_epoch_ms(std::int64_t ms) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

bool process_alive(std::int32_t pid) {
    if (pid <= 0) {
        return false;
    }
    if (::kill(pid, 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

[[noreturn]] void throw_sqlite(sqlite3* db, const std::string& context) {
    throw StoreError(context + ": " + sqlite3_errmsg(db));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw_sqlite(db_, std::string{"prepare failed: "} + sql);
        }
    }

    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bindText(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite(db_, "bind text failed");
        }
    }

    void bindInt64(int index, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
            throw_sqlite(db_, "bind int failed");
        }
    }

    void bindNull(int index) {
        if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
            throw_sqlite(db_, "bind null failed");
        }
    }

    void bindOptionalText(int index, const std::optional<std::string>& value) {
        if (value.has_value()) {
            bindText(index, *value);
        } else {
            bindNull(index);
        }
    }

    void bindOptionalInt64(int index, const std::optional<std::int64_t>& value) {
        if (value.has_value()) {
            bindInt64(index, *value);
        } else {
            bindNull(index);
        }
    }

    int step() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            const int primary = sqlite3_extended_errcode(db_) & 0xFF;
            if (primary == SQLITE_CONSTRAINT) {
                throw CorruptionError(std::string{"constraint violation: "} + sqlite3_errmsg(db_));
            }
            throw_sqlite(db_, "step failed");
        }
        return rc;
    }

    [[nodiscard]] std::string columnText(int index) const {
        const unsigned char* text = sqlite3_column_text(stmt_, index);
        if (text == nullptr) {
            return {};
        }
        const int bytes = sqlite3_column_bytes(stmt_, index);
        return std::string{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
    }

    [[nodiscard]] std::int64_t columnInt64(int index) const {
        return static_cast<std::int64_t>(sqlite3_column_int64(stmt_, index));
    }

    [[nodiscard]] bool columnIsNull(int index) const {
        return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
    }

    [[nodiscard]] std::optional<std::string> columnOptionalText(int index) const {
        if (columnIsNull(index)) {
            return std::nullopt;
        }
        return columnText(index);
    }

    [[nodiscard]] std::optional<std::int64_t> columnOptionalInt64(int index) const {
        if (columnIsNull(index)) {
            return std::nullopt;
        }
        return columnInt64(index);
    }

private:
    sqlite3*      db_   = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

void exec_sql(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc  = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string detail = message != nullptr ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        throw StoreError(std::string{"exec failed: "} + detail);
    }
}

std::string wire_type(const Event& event) {
    return std::string{wire_name(event.type)};
}

Event decode_event(std::string_view session, std::int64_t timestamp, std::string_view type,
                   std::string_view payload_text) {
    const auto parsed = parse_event_type(type);
    if (!parsed) {
        throw CorruptionError("undecodable event type: " + std::string{type});
    }
    Event event;
    event.session_id.value = std::string{session};
    event.timestamp        = from_epoch_ms(timestamp);
    event.type             = *parsed;
    event.payload          = nlohmann::json::parse(payload_text);
    return event;
}

} // namespace

class SessionPersistence::Impl {
public:
    sqlite3*          db       = nullptr;
    int               lock_fd  = -1;
    bool              writable = false;
    bool              closed   = false;
    PersistenceConfig config;
    mutable std::mutex mutex;

    static std::unique_ptr<Impl> create(const PersistenceConfig& config, bool writable);

    ~Impl() { close_noexcept(); }

    void close_noexcept() noexcept {
        if (closed) {
            return;
        }
        closed = true;
        if (db != nullptr) {
            if (writable) {
                sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
            }
            sqlite3_close_v2(db);
            db = nullptr;
        }
        if (lock_fd >= 0) {
            ::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
            lock_fd = -1;
        }
    }

    [[nodiscard]] HolderIdentity me() const { return HolderIdentity{::getpid(), config.boot_id}; }

    [[nodiscard]] bool flock_held_by_me() const noexcept { return writable && lock_fd >= 0; }

    [[nodiscard]] std::optional<SessionHeader> load_header_locked(const SessionId& id) const {
        Statement statement{
            db,
            "SELECT id, cwd, created_at, updated_at, title, model, server_profile, kind, "
            "parent_session, seed_length, metadata FROM sessions WHERE id = ?"};
        statement.bindText(1, id.value);
        if (statement.step() != SQLITE_ROW) {
            return std::nullopt;
        }
        return read_header_row(statement);
    }

    [[nodiscard]] static SessionHeader read_header_row(const Statement& statement) {
        SessionHeader header;
        header.id.value      = statement.columnText(0);
        header.cwd           = std::filesystem::path{statement.columnText(1)};
        header.createdAt     = statement.columnInt64(2);
        header.updatedAt     = statement.columnInt64(3);
        header.title         = statement.columnText(4);
        header.model         = statement.columnText(5);
        header.serverProfile = statement.columnText(6);
        header.kind = parse_session_kind(statement.columnText(7)).value_or(SessionKind::Root);
        if (const auto parent = statement.columnOptionalText(8)) {
            header.parentSession = SessionId{*parent};
        }
        if (const auto seed = statement.columnOptionalInt64(9)) {
            header.seedLength = static_cast<std::size_t>(*seed);
        }
        header.metadata = statement.columnOptionalText(10);
        return header;
    }

    // 23 §5.3: the extracted dependent-children probe shared by `erase`,
    // `eraseWithEvent`, and `hasDependents`. Caller holds `mutex`; no
    // transaction of its own so it composes into the erase transaction.
    [[nodiscard]] bool has_dependents_locked(const SessionId& id) const {
        Statement statement{db, "SELECT 1 FROM sessions WHERE parent_session = ? LIMIT 1"};
        statement.bindText(1, id.value);
        return statement.step() == SQLITE_ROW;
    }

    [[nodiscard]] EventRange read_events_after_locked(const SessionId& id, Sequence after,
                                                      std::size_t limit) const {
        if (limit == 0) {
            return {};
        }
        const bool unbounded =
            limit >= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
        Statement statement{
            db, unbounded
                    ? "SELECT sequence, session_id, event_id, timestamp, type, payload FROM events "
                      "WHERE session_id = ? AND sequence > ? ORDER BY sequence"
                    : "SELECT sequence, session_id, event_id, timestamp, type, payload FROM events "
                      "WHERE session_id = ? AND sequence > ? ORDER BY sequence LIMIT ?"};
        statement.bindText(1, id.value);
        statement.bindInt64(2, after);
        if (!unbounded) {
            statement.bindInt64(3, static_cast<std::int64_t>(limit));
        }
        EventRange records;
        while (statement.step() == SQLITE_ROW) {
            EventRecord record;
            record.seq   = statement.columnInt64(0);
            record.event = decode_event(statement.columnText(1), statement.columnInt64(3),
                                        statement.columnText(4), statement.columnText(5));
            record.event.id.value = statement.columnText(2);
            records.push_back(std::move(record));
        }
        return records;
    }

    [[nodiscard]] EventRange resolve_after_locked(const SessionId& id, Sequence after,
                                                  std::size_t limit,
                                                  std::vector<std::string>& stack) const {
        const auto header = load_header_locked(id);
        if (!header.has_value()) {
            throw UnknownSession("unknown session: " + id.value);
        }
        if (std::find(stack.begin(), stack.end(), id.value) != stack.end()) {
            throw CorruptionError("fork cycle detected at session " + id.value);
        }
        stack.push_back(id.value);

        EventRange result;
        if (header->kind == SessionKind::Fork) {
            if (!header->parentSession.has_value()) {
                throw CorruptionError("fork without parent: " + id.value);
            }
            const std::size_t seed = header->seedLength.value_or(0);
            if (seed > 0) {
                const EventRange prefix =
                    resolve_after_locked(*header->parentSession, 0, seed, stack);
                if (prefix.size() < seed) {
                    throw CorruptionError("fork seed exceeds parent view: " + id.value);
                }
                for (const EventRecord& record : prefix) {
                    if (record.seq > after) {
                        result.push_back(record);
                        if (result.size() == limit) {
                            break;
                        }
                    }
                }
            }
        }
        if (result.size() < limit) {
            EventRange own = read_events_after_locked(id, after, limit - result.size());
            result.insert(result.end(), own.begin(), own.end());
        }
        stack.pop_back();
        return result;
    }

    [[nodiscard]] EventRange resolve_locked(const SessionId& id,
                                            std::vector<std::string>& stack) const {
        return resolve_after_locked(id, 0, kUnbounded, stack);
    }

    [[nodiscard]] bool lease_matches_me_locked(const SessionId& id) const {
        Statement statement{
            db,
            "SELECT holder_pid, holder_boot_id FROM session_leases WHERE session_id = ?"};
        statement.bindText(1, id.value);
        if (statement.step() != SQLITE_ROW) {
            return false;
        }
        const HolderIdentity self = me();
        return statement.columnInt64(0) == self.pid && statement.columnText(1) == self.boot_id.value;
    }

    [[nodiscard]] bool steal_locked(const SessionId& id) {
        Statement statement{
            db,
            "SELECT holder_pid, holder_boot_id, expires_at FROM session_leases WHERE session_id = ?"};
        statement.bindText(1, id.value);
        if (statement.step() != SQLITE_ROW) {
            return false;
        }
        const std::int64_t holder_pid  = statement.columnInt64(0);
        const std::string  holder_boot = statement.columnText(1);
        const std::int64_t expires_at  = statement.columnInt64(2);
        const HolderIdentity self      = me();

        if (holder_pid == self.pid && holder_boot == self.boot_id.value) {
            return true;
        }
        if (!flock_held_by_me()) {
            return false;
        }
        const bool expired = expires_at < now_ms();
        if (!expired && process_alive(static_cast<std::int32_t>(holder_pid))) {
            return false;
        }
        Statement replace{
            db,
            "REPLACE INTO session_leases(session_id, holder_pid, holder_boot_id, acquired_at, "
            "expires_at) VALUES (?, ?, ?, ?, ?)"};
        const std::int64_t now = now_ms();
        replace.bindText(1, id.value);
        replace.bindInt64(2, self.pid);
        replace.bindText(3, self.boot_id.value);
        replace.bindInt64(4, now);
        replace.bindInt64(5, now + config.lease_ttl.count());
        replace.step();
        return true;
    }

    [[nodiscard]] bool acquire_locked(const SessionId& id) {
        const HolderIdentity self = me();
        const std::int64_t   now  = now_ms();
        Statement insert{
            db,
            "INSERT INTO session_leases(session_id, holder_pid, holder_boot_id, acquired_at, "
            "expires_at) VALUES (?, ?, ?, ?, ?) ON CONFLICT(session_id) DO NOTHING"};
        insert.bindText(1, id.value);
        insert.bindInt64(2, self.pid);
        insert.bindText(3, self.boot_id.value);
        insert.bindInt64(4, now);
        insert.bindInt64(5, now + config.lease_ttl.count());
        insert.step();
        if (sqlite3_changes(db) > 0) {
            return true;
        }
        return steal_locked(id);
    }

    [[nodiscard]] std::vector<Sequence> append_batch_locked(const SessionId& id,
                                                            std::span<const Event> events) {
        const HolderIdentity self = me();
        std::vector<Sequence> sequences;
        exec_sql(db, "BEGIN IMMEDIATE");

        bool committed = false;
        try {
            for (const Event& event : events) {
                if (event.payload.dump().size() > config.max_payload_bytes) {
                    throw PayloadTooLarge("event payload exceeds the configured cap");
                }
            }

            if (!lease_matches_me_locked(id)) {
                throw LeaseLost("not the lease holder for session " + id.value);
            }

            std::int64_t last_timestamp = 0;
            for (const Event& event : events) {
                Statement insert{
                    db,
                    "INSERT INTO events(session_id, event_id, timestamp, type, payload) VALUES (?, "
                    "?, ?, ?, ?)"};
                last_timestamp = to_epoch_ms(event.timestamp);
                insert.bindText(1, id.value);
                insert.bindText(2, event.id.value);
                insert.bindInt64(3, last_timestamp);
                insert.bindText(4, wire_type(event));
                insert.bindText(5, event.payload.dump());
                insert.step();
                sequences.push_back(static_cast<Sequence>(sqlite3_last_insert_rowid(db)));

                // 19 §5.2: materialize sessions.title in the same transaction.
                if (event.type == EventType::SessionRenamed) {
                    Statement set_title{db, "UPDATE sessions SET title = ? WHERE id = ?"};
                    set_title.bindText(1, event.payload.get<payload::SessionRenamed>().title);
                    set_title.bindText(2, id.value);
                    set_title.step();
                }
            }

            Statement update_header{db, "UPDATE sessions SET updated_at = ? WHERE id = ?"};
            update_header.bindInt64(1, last_timestamp);
            update_header.bindText(2, id.value);
            update_header.step();

            Statement renew{
                db,
                "UPDATE session_leases SET expires_at = ? WHERE session_id = ? AND holder_pid = ? "
                "AND holder_boot_id = ?"};
            renew.bindInt64(1, now_ms() + config.lease_ttl.count());
            renew.bindText(2, id.value);
            renew.bindInt64(3, self.pid);
            renew.bindText(4, self.boot_id.value);
            renew.step();

            if (!lease_matches_me_locked(id)) {
                throw LeaseLost("lease lost before commit for session " + id.value);
            }

            exec_sql(db, "COMMIT");
            committed = true;
        } catch (...) {
            if (!committed) {
                sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            }
            throw;
        }
        return sequences;
    }

    [[nodiscard]] int user_version_locked() const {
        Statement statement{db, "PRAGMA user_version"};
        if (statement.step() != SQLITE_ROW) {
            return 0;
        }
        return static_cast<int>(statement.columnInt64(0));
    }

    [[nodiscard]] int application_id_locked() const {
        Statement statement{db, "PRAGMA application_id"};
        if (statement.step() != SQLITE_ROW) {
            return 0;
        }
        return static_cast<int>(statement.columnInt64(0));
    }
};

namespace {

void apply_pragmas(sqlite3* db, const PersistenceConfig& config, bool writable) {
    exec_sql(db, "PRAGMA foreign_keys = ON");
    exec_sql(db, ("PRAGMA busy_timeout = " + std::to_string(config.busy_timeout.count())).c_str());
    exec_sql(db, "PRAGMA synchronous = NORMAL");
    if (writable) {
        exec_sql(db, ("PRAGMA wal_autocheckpoint = " +
                      std::to_string(config.wal_autocheckpoint_pages))
                         .c_str());
    }
    exec_sql(db, "PRAGMA journal_mode = WAL");
}

void migrate_fresh(sqlite3* db) {
    exec_sql(db, "BEGIN IMMEDIATE");
    try {
        exec_sql(db, kDdl);
        exec_sql(db, "PRAGMA user_version = 1");
        exec_sql(db, "PRAGMA application_id = 0x594D4801");
        exec_sql(db, "COMMIT");
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

void verify_consistency(sqlite3* db) {
    {
        Statement check{db, "PRAGMA foreign_key_check"};
        if (check.step() == SQLITE_ROW) {
            throw CorruptionError("foreign_key_check reported violations");
        }
    }
    {
        Statement check{
            db,
            "SELECT COUNT(*) FROM (SELECT session_id, sequence, LAG(sequence) OVER (PARTITION BY "
            "session_id ORDER BY sequence) AS prev FROM events) WHERE prev IS NOT NULL AND sequence "
            "<= prev"};
        if (check.step() == SQLITE_ROW && check.columnInt64(0) != 0) {
            throw CorruptionError("non-monotonic sequence in event log");
        }
    }
}

std::filesystem::path workspace_of(const PersistenceConfig& config) {
    return config.db_path.parent_path().parent_path();
}

} // namespace

std::unique_ptr<SessionPersistence::Impl> SessionPersistence::Impl::create(
    const PersistenceConfig& config, bool writable) {
    std::unique_ptr<Impl> owner{new Impl()};
    Impl&                 impl = *owner;
    impl.config                = config;
    const std::filesystem::path workspace = workspace_of(config);
    std::error_code             error;
    if (!std::filesystem::is_directory(workspace, error)) {
        throw StoreOpenError("workspace root does not exist or is not a directory: " +
                             workspace.string());
    }

    if (writable) {
        std::filesystem::create_directories(config.db_path.parent_path(), error);
        if (error) {
            throw StoreOpenError("cannot create state directory: " +
                                 config.db_path.parent_path().string());
        }
        std::filesystem::permissions(config.db_path.parent_path(),
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
    }

    if (writable) {
        impl.lock_fd = ::open(config.lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (impl.lock_fd < 0) {
            throw StoreOpenError("cannot open sidecar lock: " + config.lock_path.string());
        }
        if (::flock(impl.lock_fd, LOCK_EX | LOCK_NB) != 0) {
            const int lock_error = errno;
            ::close(impl.lock_fd);
            impl.lock_fd = -1;
            if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
                throw StoreOpenError(StoreOpenErrorCode::Locked,
                                     "workspace is locked by another daemon: " +
                                         config.lock_path.string());
            }
            throw StoreOpenError(StoreOpenErrorCode::Unavailable,
                                 "cannot acquire sidecar lock: " + config.lock_path.string());
        }
    }

    const int flags = writable ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX)
                               : (SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX);
    if (sqlite3_open_v2(config.db_path.c_str(), &impl.db, flags, nullptr) != SQLITE_OK) {
        const std::string message = impl.db != nullptr ? sqlite3_errmsg(impl.db) : "open failed";
        if (impl.db != nullptr) {
            sqlite3_close_v2(impl.db);
            impl.db = nullptr;
        }
        if (impl.lock_fd >= 0) {
            ::flock(impl.lock_fd, LOCK_UN);
            ::close(impl.lock_fd);
            impl.lock_fd = -1;
        }
        throw StoreOpenError("cannot open database: " + message);
    }

    impl.writable = writable;
    apply_pragmas(impl.db, config, writable);

    const int app_id = impl.application_id_locked();
    if (app_id != 0 && app_id != kApplicationId) {
        throw SchemaVersionError("database is not a ymh sessions.db");
    }

    const int version = impl.user_version_locked();
    if (writable) {
        if (version == 0) {
            migrate_fresh(impl.db);
        } else if (version > kSchemaVersion) {
            throw SchemaVersionError("database schema is newer than this binary");
        } else if (version < kSchemaVersion) {
            throw SchemaVersionError("database schema predates this binary");
        }
    } else if (version != kSchemaVersion) {
        throw SchemaVersionError("database schema does not match this binary");
    }

    verify_consistency(impl.db);

    if (writable) {
        const std::string diagnostic =
            nlohmann::json{
                {"pid", ::getpid()},
                {"boot_id", config.boot_id.value},
                {"acquired_at", now_ms()},
            }
                .dump();
        if (::ftruncate(impl.lock_fd, 0) == 0) {
            const ssize_t ignored =
                ::write(impl.lock_fd, diagnostic.data(), diagnostic.size());
            (void)ignored;
        }
    }

    return owner;
}

SessionPersistence::SessionPersistence(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SessionPersistence::~SessionPersistence() = default;

std::unique_ptr<SessionPersistence> SessionPersistence::open(const PersistenceConfig& config) {
    return std::unique_ptr<SessionPersistence>{new SessionPersistence(Impl::create(config, true))};
}

std::unique_ptr<SessionPersistence> SessionPersistence::openReadOnly(
    const PersistenceConfig& config) {
    return std::unique_ptr<SessionPersistence>{
        new SessionPersistence(Impl::create(config, false))};
}

SessionHeader SessionPersistence::create(SessionHeader header) {
    validateHeader(header);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    exec_sql(impl_->db, "BEGIN IMMEDIATE");
    try {
        Statement insert{
            impl_->db,
            "INSERT INTO sessions(id, cwd, created_at, updated_at, title, model, server_profile, "
            "kind, parent_session, seed_length, metadata) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"};
        insert.bindText(1, header.id.value);
        insert.bindText(2, header.cwd.string());
        insert.bindInt64(3, header.createdAt);
        insert.bindInt64(4, header.updatedAt);
        insert.bindText(5, header.title);
        insert.bindText(6, header.model);
        insert.bindText(7, header.serverProfile);
        insert.bindText(8, std::string{session_kind_name(header.kind)});
        insert.bindOptionalText(9, header.parentSession.has_value()
                                       ? std::optional<std::string>{header.parentSession->value}
                                       : std::nullopt);
        insert.bindOptionalInt64(
            10, header.seedLength.has_value()
                    ? std::optional<std::int64_t>{static_cast<std::int64_t>(*header.seedLength)}
                    : std::nullopt);
        insert.bindOptionalText(11, header.metadata);
        insert.step();

        if (!impl_->acquire_locked(header.id)) {
            throw StoreError("cannot acquire lease for new session " + header.id.value);
        }
        exec_sql(impl_->db, "COMMIT");
    } catch (...) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    return header;
}

std::optional<SessionHeader> SessionPersistence::load(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->load_header_locked(id);
}

std::vector<SessionHeader> SessionPersistence::list() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement statement{
        impl_->db,
        "SELECT id, cwd, created_at, updated_at, title, model, server_profile, kind, "
        "parent_session, seed_length, metadata FROM sessions ORDER BY created_at, id"};
    std::vector<SessionHeader> headers;
    while (statement.step() == SQLITE_ROW) {
        headers.push_back(Impl::read_header_row(statement));
    }
    return headers;
}

void SessionPersistence::erase(SessionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    exec_sql(impl_->db, "BEGIN IMMEDIATE");
    try {
        if (impl_->has_dependents_locked(id)) {
            throw DependentSessionError("session has dependent children: " + id.value);
        }
        {
            Statement snapshots{impl_->db, "DELETE FROM session_snapshots WHERE session_id = ?"};
            snapshots.bindText(1, id.value);
            snapshots.step();
        }
        {
            Statement leases{impl_->db, "DELETE FROM session_leases WHERE session_id = ?"};
            leases.bindText(1, id.value);
            leases.step();
        }
        {
            Statement events{impl_->db, "DELETE FROM events WHERE session_id = ?"};
            events.bindText(1, id.value);
            events.step();
        }
        {
            Statement sessions{impl_->db, "DELETE FROM sessions WHERE id = ?"};
            sessions.bindText(1, id.value);
            sessions.step();
        }
        exec_sql(impl_->db, "COMMIT");
    } catch (...) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

bool SessionPersistence::isUnprompted(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement statement{
        impl_->db,
        "SELECT 1 FROM events WHERE session_id = ? AND type = 'user/message' LIMIT 1"};
    statement.bindText(1, id.value);
    return statement.step() != SQLITE_ROW;
}

bool SessionPersistence::hasDependents(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->has_dependents_locked(id);
}

void SessionPersistence::eraseWithEvent(SessionId id, Event event) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    exec_sql(impl_->db, "BEGIN IMMEDIATE");
    try {
        if (impl_->has_dependents_locked(id)) {
            throw DependentSessionError("session has dependent children: " + id.value);
        }
        if (event.payload.dump().size() > impl_->config.max_payload_bytes) {
            throw PayloadTooLarge("event payload exceeds the configured cap");
        }
        {
            Statement insert{
                impl_->db,
                "INSERT INTO events(session_id, event_id, timestamp, type, payload) VALUES (?, ?, "
                "?, ?, ?)"};
            insert.bindText(1, id.value);
            insert.bindText(2, event.id.value);
            insert.bindInt64(3, to_epoch_ms(event.timestamp));
            insert.bindText(4, wire_type(event));
            insert.bindText(5, event.payload.dump());
            insert.step();
        }
        {
            Statement snapshots{impl_->db, "DELETE FROM session_snapshots WHERE session_id = ?"};
            snapshots.bindText(1, id.value);
            snapshots.step();
        }
        {
            Statement leases{impl_->db, "DELETE FROM session_leases WHERE session_id = ?"};
            leases.bindText(1, id.value);
            leases.step();
        }
        {
            Statement events{impl_->db, "DELETE FROM events WHERE session_id = ?"};
            events.bindText(1, id.value);
            events.step();
        }
        {
            Statement sessions{impl_->db, "DELETE FROM sessions WHERE id = ?"};
            sessions.bindText(1, id.value);
            sessions.step();
        }
        exec_sql(impl_->db, "COMMIT");
    } catch (...) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

EventRange SessionPersistence::read(SessionId id, Sequence after) const {
    return readAfter(id, after, kUnbounded);
}

Sequence SessionPersistence::headSequence(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto header = impl_->load_header_locked(id);
    if (!header.has_value()) {
        throw UnknownSession("unknown session: " + id.value);
    }
    Statement statement{
        impl_->db, "SELECT COALESCE(MAX(sequence), 0) FROM events WHERE session_id = ?"};
    statement.bindText(1, id.value);
    Sequence head = 0;
    if (statement.step() == SQLITE_ROW) {
        head = statement.columnInt64(0);
    }
    if (head == 0 && header->kind == SessionKind::Fork) {
        std::vector<std::string> stack;
        for (const EventRecord& record : impl_->resolve_locked(id, stack)) {
            head = std::max(head, record.seq);
        }
    }
    return head;
}

EventRange SessionPersistence::readAfter(SessionId id, Sequence after, std::size_t limit) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (limit == 0) {
        return {};
    }
    std::vector<std::string> stack;
    return impl_->resolve_after_locked(id, after, limit, stack);
}

EventRange SessionPersistence::readRange(SessionId id, Sequence from, Sequence to) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::string>    stack;
    EventRange                  resolved = impl_->resolve_locked(id, stack);
    EventRange                  result;
    for (const EventRecord& record : resolved) {
        if (record.seq >= from && record.seq <= to) {
            result.push_back(record);
        }
    }
    return result;
}

Sequence SessionPersistence::append(SessionId id, Event event) {
    const Event single[] = {std::move(event)};
    const std::vector<Sequence> sequences = appendBatch(id, single);
    return sequences.front();
}

std::vector<Sequence> SessionPersistence::appendBatch(SessionId id,
                                                      std::span<const Event> events) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    if (events.empty()) {
        return {};
    }
    return impl_->append_batch_locked(id, events);
}

bool SessionPersistence::isLeaseHolder(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        return false;
    }
    return impl_->lease_matches_me_locked(id);
}

LeaseState SessionPersistence::leaseState(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement statement{
        impl_->db,
        "SELECT holder_pid, holder_boot_id, expires_at FROM session_leases WHERE session_id = ?"};
    statement.bindText(1, id.value);
    if (statement.step() != SQLITE_ROW) {
        return LeaseState::Absent;
    }
    const HolderIdentity self = impl_->me();
    if (statement.columnInt64(0) == self.pid && statement.columnText(1) == self.boot_id.value) {
        return LeaseState::HeldByMe;
    }
    if (statement.columnInt64(2) < now_ms()) {
        return LeaseState::Expired;
    }
    return LeaseState::HeldByOther;
}

bool SessionPersistence::acquireLease(SessionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    return impl_->acquire_locked(id);
}

void SessionPersistence::renewLeases() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    const HolderIdentity self = impl_->me();
    Statement renew{
        impl_->db,
        "UPDATE session_leases SET expires_at = ? WHERE holder_pid = ? AND holder_boot_id = ?"};
    renew.bindInt64(1, now_ms() + impl_->config.lease_ttl.count());
    renew.bindInt64(2, self.pid);
    renew.bindText(3, self.boot_id.value);
    renew.step();
}

bool SessionPersistence::releaseLease(SessionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    const HolderIdentity self = impl_->me();
    Statement statement{
        impl_->db,
        "DELETE FROM session_leases WHERE session_id = ? AND holder_pid = ? AND holder_boot_id = "
        "?"};
    statement.bindText(1, id.value);
    statement.bindInt64(2, self.pid);
    statement.bindText(3, self.boot_id.value);
    statement.step();
    return true;
}

void SessionPersistence::checkpoint(SessionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    const auto header = impl_->load_header_locked(id);
    if (!header.has_value()) {
        throw UnknownSession("unknown session: " + id.value);
    }
    std::vector<std::string> stack;
    const EventRange         resolved = impl_->resolve_locked(id, stack);
    const std::vector<Message> messages = deriveMessages(*header, resolved);
    const Sequence             head     = resolved.empty() ? 0 : resolved.back().seq;

    exec_sql(impl_->db, "BEGIN IMMEDIATE");
    try {
        Statement statement{
            impl_->db,
            "REPLACE INTO session_snapshots(session_id, at_sequence, header_json, messages_json, "
            "event_count, created_at) VALUES (?, ?, ?, ?, ?, ?)"};
        statement.bindText(1, id.value);
        statement.bindInt64(2, head);
        statement.bindText(3, nlohmann::json(*header).dump());
        statement.bindText(4, nlohmann::json(messages).dump());
        statement.bindInt64(5, static_cast<std::int64_t>(resolved.size()));
        statement.bindInt64(6, now_ms());
        statement.step();
        exec_sql(impl_->db, "COMMIT");
    } catch (...) {
        sqlite3_exec(impl_->db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::optional<SessionSnapshot> SessionPersistence::loadSnapshot(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement statement{
        impl_->db,
        "SELECT at_sequence, header_json, messages_json, event_count FROM session_snapshots WHERE "
        "session_id = ?"};
    statement.bindText(1, id.value);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    try {
        SessionSnapshot snapshot;
        snapshot.session    = id;
        snapshot.at         = statement.columnInt64(0);
        snapshot.header     = nlohmann::json::parse(statement.columnText(1)).get<SessionHeader>();
        snapshot.messages =
            nlohmann::json::parse(statement.columnText(2)).get<std::vector<Message>>();
        snapshot.eventCount = static_cast<std::size_t>(statement.columnInt64(3));
        return snapshot;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool SessionPersistence::snapshotIsCurrent(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Statement statement{impl_->db, "SELECT at_sequence FROM session_snapshots WHERE session_id = ?"};
    statement.bindText(1, id.value);
    if (statement.step() != SQLITE_ROW) {
        return false;
    }
    const std::int64_t at = statement.columnInt64(0);
    std::vector<std::string> stack;
    const EventRange resolved = impl_->resolve_locked(id, stack);
    const Sequence   head     = resolved.empty() ? 0 : resolved.back().seq;
    return at == head;
}

void SessionPersistence::discardSnapshot(SessionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    Statement statement{impl_->db, "DELETE FROM session_snapshots WHERE session_id = ?"};
    statement.bindText(1, id.value);
    statement.step();
}

int SessionPersistence::schemaVersion() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->user_version_locked();
}

void SessionPersistence::close() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->close_noexcept();
}

SqliteSessionHandle::SqliteSessionHandle(SessionPersistence& store, SessionId id, bool writable)
    : store_(store), id_(std::move(id)), writable_(writable) {}

SqliteSessionHandle::~SqliteSessionHandle() {
    release();
}

void SqliteSessionHandle::release() {
    if (!writable_ || released_) {
        return;
    }
    released_ = true;
    store_.releaseLease(id_);
}

} // namespace ymh

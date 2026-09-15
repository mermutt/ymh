#include "ymh/registry/registry.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "ymh/registry/liveness.hpp"

namespace ymh {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

constexpr char kRegistryDdl[] = R"sql(
CREATE TABLE workspaces (
    id              TEXT PRIMARY KEY,
    canonical_path  TEXT NOT NULL UNIQUE,
    display_title   TEXT NOT NULL,
    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL,
    host_pid        INTEGER,
    host_boot_id    TEXT,
    host_socket     TEXT,
    host_heartbeat  INTEGER,
    metadata        JSON,
    CHECK (host_pid IS NULL OR host_pid > 0),
    CHECK ((host_pid IS NULL) = (host_boot_id IS NULL)),
    CHECK ((host_pid IS NULL) = (host_socket IS NULL)),
    CHECK ((host_pid IS NULL) = (host_heartbeat IS NULL))
);

CREATE TABLE workspace_sessions (
    workspace_id    TEXT NOT NULL,
    session_id      TEXT NOT NULL,
    ordinal         INTEGER NOT NULL,
    archived        INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    PRIMARY KEY (workspace_id, session_id),
    FOREIGN KEY (workspace_id) REFERENCES workspaces(id),
    CHECK (archived IN (0, 1))
);

CREATE INDEX idx_ws_sessions ON workspace_sessions(workspace_id, ordinal);

CREATE TABLE pending_mutation (
    workspace_id    TEXT PRIMARY KEY,
    mutation_type   TEXT NOT NULL,
    payload         JSON NOT NULL,
    timestamp       INTEGER NOT NULL,
    CHECK (mutation_type IN ('create','delete','reorder'))
);

CREATE TABLE registry_meta (
    key             TEXT PRIMARY KEY,
    value           TEXT NOT NULL
);
)sql";

[[noreturn]] void throw_registry(RegistryErrorCode code, const std::string& message) {
    throw RegistryError(code, message);
}

[[noreturn]] void throw_sqlite(sqlite3* db, RegistryErrorCode code,
                               const std::string& context) {
    const char* detail = db != nullptr ? sqlite3_errmsg(db) : "no connection";
    throw RegistryError(code, context + ": " + detail);
}

void exec_sql(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string detail = message != nullptr ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        throw RegistryError(RegistryErrorCode::Corrupt, std::string{"sql failed: "} + detail);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw_sqlite(db_, RegistryErrorCode::Corrupt, std::string{"prepare failed: "} + sql);
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
            throw_sqlite(db_, RegistryErrorCode::Corrupt, "bind text failed");
        }
    }

    void bindInt64(int index, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
            throw_sqlite(db_, RegistryErrorCode::Corrupt, "bind int failed");
        }
    }

    void bindNull(int index) {
        if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
            throw_sqlite(db_, RegistryErrorCode::Corrupt, "bind null failed");
        }
    }

    void bindOptionalText(int index, const std::optional<std::string>& value) {
        if (value.has_value()) {
            bindText(index, *value);
        } else {
            bindNull(index);
        }
    }

    int step() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            const int primary = sqlite3_extended_errcode(db_) & 0xFF;
            if (primary == SQLITE_CONSTRAINT) {
                throw_sqlite(db_, RegistryErrorCode::Corrupt, "constraint violation");
            }
            throw_sqlite(db_, RegistryErrorCode::Corrupt, "statement step failed");
        }
        return rc;
    }

    [[nodiscard]] bool isNull(int index) const {
        return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
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

private:
    sqlite3*      db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec_sql(db_, "BEGIN IMMEDIATE"); }

    ~Transaction() {
        if (active_) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        exec_sql(db_, "COMMIT");
        active_ = false;
    }

private:
    sqlite3* db_ = nullptr;
    bool     active_ = true;
};

void apply_pragmas(sqlite3* db, const RegistryConfig& config, bool writable) {
    exec_sql(db, "PRAGMA foreign_keys = ON");
    exec_sql(db, ("PRAGMA busy_timeout = " + std::to_string(config.busy_timeout.count())).c_str());
    exec_sql(db, "PRAGMA synchronous = NORMAL");
    if (writable) {
        exec_sql(db, "PRAGMA wal_autocheckpoint = 1000");
    }
    exec_sql(db, "PRAGMA journal_mode = WAL");
}

void migrate_fresh(sqlite3* db) {
    Transaction transaction{db};
    exec_sql(db, kRegistryDdl);
    exec_sql(db, "PRAGMA user_version = 1");
    exec_sql(db, "PRAGMA application_id = 0x594D4802");
    transaction.commit();
}

int read_pragma_int(sqlite3* db, const char* pragma) {
    Statement statement{db, pragma};
    if (statement.step() != SQLITE_ROW) {
        return 0;
    }
    return static_cast<int>(statement.columnInt64(0));
}

void verify_foreign_keys(sqlite3* db) {
    Statement check{db, "PRAGMA foreign_key_check"};
    if (check.step() == SQLITE_ROW) {
        throw_registry(RegistryErrorCode::Corrupt, "foreign_key_check reported violations");
    }
}

std::filesystem::path expand_path(const std::filesystem::path& path) {
    std::string value = path.string();
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        if (!value.empty() && value.front() == '~' &&
            (value.size() == 1 || value[1] == '/')) {
            value = std::string{home} + value.substr(1);
        } else if (value.rfind("$HOME", 0) == 0) {
            value = std::string{home} + value.substr(5);
        }
    }
    return std::filesystem::path{value};
}

const std::array<std::string_view, 7> kDenylist{".git", "node_modules", ".cache",
                                                "Library", "target", "build", ".venv"};

bool is_denied(const std::filesystem::path& directory) {
    const std::string name = directory.filename().string();
    return std::any_of(kDenylist.begin(), kDenylist.end(),
                       [&name](std::string_view denied) { return name == denied; });
}

std::string basename_title(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    if (name.empty()) {
        name = path.string();
    }
    return name;
}

nlohmann::json prev_ordinals_locked(sqlite3* db, const WorkspaceId& workspace) {
    nlohmann::json result = nlohmann::json::object();
    Statement statement{
        db, "SELECT session_id, ordinal FROM workspace_sessions WHERE workspace_id = ?"};
    statement.bindText(1, workspace.value);
    while (statement.step() == SQLITE_ROW) {
        result[statement.columnText(0)] = statement.columnInt64(1);
    }
    return result;
}

WorkspaceRecord read_workspace(Statement& statement) {
    WorkspaceRecord record;
    record.id = WorkspaceId{statement.columnText(0)};
    record.canonicalPath = std::filesystem::path{statement.columnText(1)};
    record.displayTitle = statement.columnText(2);
    record.createdAt = statement.columnInt64(3);
    record.updatedAt = statement.columnInt64(4);
    if (!statement.isNull(5)) {
        HostClaim claim;
        claim.workspace = record.id;
        claim.pid = static_cast<HostPid>(statement.columnInt64(5));
        claim.bootId = HostBootId{statement.columnText(6)};
        claim.socketPath = std::filesystem::path{statement.columnText(7)};
        record.host = std::move(claim);
        record.heartbeatAt = statement.columnInt64(8);
    }
    if (!statement.isNull(9)) {
        record.metadata = statement.columnText(9);
    }
    return record;
}

constexpr char kSelectWorkspaceColumns[] =
    "SELECT id, canonical_path, display_title, created_at, updated_at, host_pid, "
    "host_boot_id, host_socket, host_heartbeat, metadata FROM workspaces";

WorkspaceSessionRecord read_session(Statement& statement) {
    WorkspaceSessionRecord record;
    record.workspace = WorkspaceId{statement.columnText(0)};
    record.sessionId = SessionId{statement.columnText(1)};
    record.ordinal = statement.columnInt64(2);
    record.archived = statement.columnInt64(3) != 0;
    record.createdAt = statement.columnInt64(4);
    return record;
}

std::string mutation_name(MutationType type) {
    return std::string{mutation_type_name(type)};
}

bool marker_exists(sqlite3* db, const WorkspaceId& workspace) {
    Statement statement{db, "SELECT 1 FROM pending_mutation WHERE workspace_id = ?"};
    statement.bindText(1, workspace.value);
    return statement.step() == SQLITE_ROW;
}

void insert_marker(sqlite3* db, const WorkspaceId& workspace, MutationType type,
                   const std::string& payload) {
    Transaction transaction{db};
    Statement statement{
        db,
        "INSERT INTO pending_mutation(workspace_id, mutation_type, payload, timestamp) "
        "VALUES (?, ?, ?, ?)"};
    statement.bindText(1, workspace.value);
    statement.bindText(2, mutation_name(type));
    statement.bindText(3, payload);
    statement.bindInt64(4, now_ms());
    statement.step();
    transaction.commit();
}

void delete_marker(sqlite3* db, const WorkspaceId& workspace) {
    Statement statement{db, "DELETE FROM pending_mutation WHERE workspace_id = ?"};
    statement.bindText(1, workspace.value);
    statement.step();
}

void touch_workspace(sqlite3* db, const WorkspaceId& workspace) {
    Statement statement{db, "UPDATE workspaces SET updated_at = ? WHERE id = ?"};
    statement.bindInt64(1, now_ms());
    statement.bindText(2, workspace.value);
    statement.step();
}

void ensure_workspace_exists(sqlite3* db, const WorkspaceId& workspace) {
    Statement statement{db, "SELECT 1 FROM workspaces WHERE id = ?"};
    statement.bindText(1, workspace.value);
    if (statement.step() != SQLITE_ROW) {
        throw_registry(RegistryErrorCode::UnknownWorkspace,
                       "no workspace row for id " + workspace.value);
    }
}

void validate_payload(const nlohmann::json& payload, MutationType type,
                      const WorkspaceId& workspace) {
    const auto require_string = [&](const char* key) {
        if (!payload.contains(key) || payload.at(key).type() != nlohmann::json::value_t::string) {
            throw_registry(RegistryErrorCode::Corrupt,
                           "malformed pending_mutation payload for " + workspace.value);
        }
    };
    const auto require_integer = [&](const char* key) {
        if (!payload.contains(key) ||
            (payload.at(key).type() != nlohmann::json::value_t::number_integer &&
             payload.at(key).type() != nlohmann::json::value_t::number_unsigned)) {
            throw_registry(RegistryErrorCode::Corrupt,
                           "malformed pending_mutation payload for " + workspace.value);
        }
    };
    require_string("session_id");
    if (!payload.contains("prev_ordinals") ||
        payload.at("prev_ordinals").type() != nlohmann::json::value_t::object) {
        throw_registry(RegistryErrorCode::Corrupt,
                       "malformed pending_mutation payload for " + workspace.value);
    }
    if (type == MutationType::Create) {
        require_integer("new_ordinal");
    } else if (type == MutationType::Reorder) {
        require_integer("from_ordinal");
        require_integer("to_ordinal");
    }
}

void restore_prev_ordinals(sqlite3* db, const WorkspaceId& workspace,
                           const nlohmann::json& prev_ordinals) {
    for (const auto& [session, ordinal] : prev_ordinals.items()) {
        Statement statement{
            db,
            "UPDATE workspace_sessions SET ordinal = ? WHERE workspace_id = ? AND session_id = ?"};
        statement.bindInt64(1, ordinal.get<std::int64_t>());
        statement.bindText(2, workspace.value);
        statement.bindText(3, session);
        statement.step();
    }
}

void apply_reorder(sqlite3* db, const WorkspaceId& workspace, const std::string& session,
                   std::int64_t from_ordinal, std::int64_t to_ordinal) {
    if (from_ordinal == to_ordinal) {
        return;
    }
    if (from_ordinal < to_ordinal) {
        Statement shift{
            db,
            "UPDATE workspace_sessions SET ordinal = ordinal - 1 WHERE workspace_id = ? AND "
            "ordinal > ? AND ordinal <= ? AND session_id != ?"};
        shift.bindText(1, workspace.value);
        shift.bindInt64(2, from_ordinal);
        shift.bindInt64(3, to_ordinal);
        shift.bindText(4, session);
        shift.step();
    } else {
        Statement shift{
            db,
            "UPDATE workspace_sessions SET ordinal = ordinal + 1 WHERE workspace_id = ? AND "
            "ordinal >= ? AND ordinal < ? AND session_id != ?"};
        shift.bindText(1, workspace.value);
        shift.bindInt64(2, to_ordinal);
        shift.bindInt64(3, from_ordinal);
        shift.bindText(4, session);
        shift.step();
    }
    Statement move{db,
                   "UPDATE workspace_sessions SET ordinal = ? WHERE workspace_id = ? AND "
                   "session_id = ?"};
    move.bindInt64(1, to_ordinal);
    move.bindText(2, workspace.value);
    move.bindText(3, session);
    move.step();
}

} // namespace

struct WorkspaceRegistry::Impl {
    RegistryConfig         config;
    sqlite3*               db = nullptr;
    int                    lock_fd = -1;
    bool                   writable = false;
    bool                   write_lock_held = false;
    int                    write_lock_depth = 0;
    std::recursive_mutex   mutex;

    ~Impl() {
        if (db != nullptr) {
            sqlite3_close_v2(db);
            db = nullptr;
        }
        if (lock_fd >= 0) {
            ::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
            lock_fd = -1;
        }
    }

    class LockGuard {
    public:
        explicit LockGuard(Impl& impl) : impl_(impl) {
            impl_.mutex.lock();
            try {
                impl_.acquire_write_lock();
            } catch (...) {
                impl_.mutex.unlock();
                throw;
            }
        }

        ~LockGuard() {
            impl_.release_write_lock();
            impl_.mutex.unlock();
        }

        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;

    private:
        Impl& impl_;
    };

    void acquire_write_lock();
    void release_write_lock();
    void write_lock_diagnostic();
};

void WorkspaceRegistry::Impl::acquire_write_lock() {
    if (!writable) {
        throw_registry(RegistryErrorCode::NotWriteLockHolder,
                       "read-only registry cannot perform a mutation");
    }
    if (write_lock_depth > 0) {
        ++write_lock_depth;
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + config.lock_retry_budget;
    while (true) {
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) == 0) {
            break;
        }
        const int error = errno;
        if (error != EWOULDBLOCK && error != EINTR) {
            throw_registry(RegistryErrorCode::LockUnavailable,
                           std::string{"flock(LOCK_EX) failed: "} + std::strerror(error));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw_registry(RegistryErrorCode::LockUnavailable,
                           "registry write lock unavailable after retry budget");
        }
        std::this_thread::sleep_for(config.lock_retry_interval);
    }
    write_lock_depth = 1;
    write_lock_held = true;
    write_lock_diagnostic();
}

void WorkspaceRegistry::Impl::release_write_lock() {
    if (write_lock_depth <= 0) {
        return;
    }
    --write_lock_depth;
    if (write_lock_depth == 0) {
        ::flock(lock_fd, LOCK_UN);
        write_lock_held = false;
    }
}

void WorkspaceRegistry::Impl::write_lock_diagnostic() {
    const std::string diagnostic =
        nlohmann::json{{"pid", ::getpid()}, {"acquired_at", now_ms()}}.dump();
    if (::ftruncate(lock_fd, 0) == 0) {
        const ssize_t ignored = ::write(lock_fd, diagnostic.data(), diagnostic.size());
        (void)ignored;
    }
}

std::string generate_uuid_v4() {
    std::array<unsigned char, 16> bytes{};
    std::random_device            device;
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(device() & 0xFF);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    return std::format(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

std::string_view mutation_type_name(MutationType type) noexcept {
    switch (type) {
        case MutationType::Create:
            return "create";
        case MutationType::Delete:
            return "delete";
        case MutationType::Reorder:
            return "reorder";
    }
    return {};
}

std::optional<MutationType> parse_mutation_type(std::string_view name) noexcept {
    if (name == "create") {
        return MutationType::Create;
    }
    if (name == "delete") {
        return MutationType::Delete;
    }
    if (name == "reorder") {
        return MutationType::Reorder;
    }
    return std::nullopt;
}

std::filesystem::path default_state_dir() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path{xdg} / "ymh";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".local" / "state" / "ymh";
    }
    return std::filesystem::path{".local"} / "state" / "ymh";
}

std::filesystem::path default_registry_db_path() { return default_state_dir() / "registry.db"; }

std::filesystem::path default_registry_lock_path() {
    return default_state_dir() / "registry.lock";
}

void ensure_state_dir(const std::filesystem::path& dir) {
    std::error_code error;
    if (dir.empty()) {
        return;
    }
    std::filesystem::create_directories(dir, error);
    if (error) {
        throw_registry(RegistryErrorCode::OpenFailed,
                       "cannot create state directory: " + dir.string());
    }
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
        throw_registry(RegistryErrorCode::OpenFailed,
                       "cannot set permissions on state directory: " + dir.string());
    }
}

RegistryConfig default_registry_config() {
    RegistryConfig config;
    config.db_path = default_registry_db_path();
    config.lock_path = default_registry_lock_path();
    return config;
}

WorkspaceRegistry::WorkspaceRegistry(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WorkspaceRegistry::~WorkspaceRegistry() = default;

std::unique_ptr<WorkspaceRegistry> WorkspaceRegistry::open(const RegistryConfig& config) {
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->writable = true;

    ensure_state_dir(config.db_path.parent_path());

    impl->lock_fd = ::open(config.lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (impl->lock_fd < 0) {
        throw_registry(RegistryErrorCode::OpenFailed,
                       "cannot open registry lock file: " + config.lock_path.string());
    }
    if (::fchmod(impl->lock_fd, 0600) != 0) {
        throw_registry(RegistryErrorCode::OpenFailed,
                       "cannot set registry lock permissions: " + config.lock_path.string());
    }

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(config.db_path.c_str(), &impl->db, flags, nullptr) != SQLITE_OK) {
        throw_sqlite(impl->db, RegistryErrorCode::OpenFailed, "cannot open registry database");
    }
    apply_pragmas(impl->db, config, true);

    const int application_id = read_pragma_int(impl->db, "PRAGMA application_id");
    if (application_id != 0 && application_id != kRegistryApplicationId) {
        throw_registry(RegistryErrorCode::SchemaVersion,
                       "registry.db has a foreign application_id");
    }
    const int version = read_pragma_int(impl->db, "PRAGMA user_version");
    if (version == 0) {
        migrate_fresh(impl->db);
    } else if (version > kRegistrySchemaVersion) {
        throw_registry(RegistryErrorCode::SchemaVersion,
                       "registry.db was written by a newer binary");
    } else if (version < kRegistrySchemaVersion) {
        throw_registry(RegistryErrorCode::SchemaVersion,
                       "registry.db schema has no migration path to this binary");
    }
    verify_foreign_keys(impl->db);

    auto registry = std::unique_ptr<WorkspaceRegistry>{new WorkspaceRegistry(std::move(impl))};
    registry->resolvePendingMutations();
    if (!registry->isInitialized()) {
        registry->bootstrap();
    }
    return registry;
}

std::unique_ptr<WorkspaceRegistry> WorkspaceRegistry::openReadOnly(const RegistryConfig& config) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(config.db_path, error)) {
        throw_registry(RegistryErrorCode::OpenFailed,
                       "registry database is missing: " + config.db_path.string());
    }

    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->writable = false;

    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(config.db_path.c_str(), &impl->db, flags, nullptr) != SQLITE_OK) {
        throw_sqlite(impl->db, RegistryErrorCode::OpenFailed, "cannot open registry database");
    }
    apply_pragmas(impl->db, config, false);

    const int application_id = read_pragma_int(impl->db, "PRAGMA application_id");
    const int version = read_pragma_int(impl->db, "PRAGMA user_version");
    if (application_id != kRegistryApplicationId || version != kRegistrySchemaVersion) {
        throw_registry(RegistryErrorCode::SchemaVersion,
                       "registry.db is foreign or has an unexpected schema version");
    }
    verify_foreign_keys(impl->db);

    auto registry = std::unique_ptr<WorkspaceRegistry>{new WorkspaceRegistry(std::move(impl))};
    if (!registry->isInitialized()) {
        throw_registry(RegistryErrorCode::Uninitialized,
                       "registry.db predates the `initialized` marker");
    }
    return registry;
}

bool WorkspaceRegistry::isInitialized() const {
    return metaValue(kInitializedMarker).has_value();
}

std::optional<std::string> WorkspaceRegistry::metaValue(std::string_view key) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement statement{impl_->db, "SELECT value FROM registry_meta WHERE key = ?"};
    statement.bindText(1, std::string{key});
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return statement.columnText(0);
}

std::vector<WorkspaceRecord> WorkspaceRegistry::listWorkspaces() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    std::vector<WorkspaceRecord> result;
    Statement statement{impl_->db, (std::string{kSelectWorkspaceColumns} +
                                    " ORDER BY canonical_path ASC")
                                       .c_str()};
    while (statement.step() == SQLITE_ROW) {
        result.push_back(read_workspace(statement));
    }
    return result;
}

std::optional<WorkspaceRecord> WorkspaceRegistry::findById(WorkspaceId id) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement statement{impl_->db, (std::string{kSelectWorkspaceColumns} + " WHERE id = ?").c_str()};
    statement.bindText(1, id.value);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return read_workspace(statement);
}

std::optional<WorkspaceRecord> WorkspaceRegistry::findByCanonicalPath(
    const std::filesystem::path& path) const {
    std::error_code error;
    std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error) {
        canonical = path;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement statement{
        impl_->db,
        (std::string{kSelectWorkspaceColumns} + " WHERE canonical_path = ?").c_str()};
    statement.bindText(1, canonical.string());
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return read_workspace(statement);
}

std::vector<WorkspaceSessionRecord> WorkspaceRegistry::listSessions(WorkspaceId workspace) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    std::vector<WorkspaceSessionRecord> result;
    Statement statement{
        impl_->db,
        "SELECT workspace_id, session_id, ordinal, archived, created_at FROM workspace_sessions "
        "WHERE workspace_id = ? ORDER BY ordinal ASC"};
    statement.bindText(1, workspace.value);
    while (statement.step() == SQLITE_ROW) {
        result.push_back(read_session(statement));
    }
    return result;
}

std::optional<WorkspaceSessionRecord> WorkspaceRegistry::findSession(WorkspaceId workspace,
                                                                     SessionId session) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement statement{
        impl_->db,
        "SELECT workspace_id, session_id, ordinal, archived, created_at FROM workspace_sessions "
        "WHERE workspace_id = ? AND session_id = ?"};
    statement.bindText(1, workspace.value);
    statement.bindText(2, session.value);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    return read_session(statement);
}

std::optional<PendingMutation> WorkspaceRegistry::pendingMutation(WorkspaceId workspace) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement statement{
        impl_->db,
        "SELECT workspace_id, mutation_type, payload, timestamp FROM pending_mutation WHERE "
        "workspace_id = ?"};
    statement.bindText(1, workspace.value);
    if (statement.step() != SQLITE_ROW) {
        return std::nullopt;
    }
    PendingMutation marker;
    marker.workspace = WorkspaceId{statement.columnText(0)};
    const auto type = parse_mutation_type(statement.columnText(1));
    if (!type.has_value()) {
        throw_registry(RegistryErrorCode::Corrupt, "unknown pending_mutation type");
    }
    marker.type = *type;
    marker.payload = statement.columnText(2);
    marker.timestamp = statement.columnInt64(3);
    return marker;
}

std::size_t WorkspaceRegistry::liveHostCount() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    Statement statement{impl_->db, "SELECT COUNT(*) FROM workspaces WHERE host_pid IS NOT NULL"};
    if (statement.step() != SQLITE_ROW) {
        return 0;
    }
    return static_cast<std::size_t>(statement.columnInt64(0));
}

HostLiveness WorkspaceRegistry::probeLiveness(WorkspaceId workspace) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    const auto record = findById(workspace);
    if (!record.has_value() || !record->host.has_value()) {
        return HostLiveness::Absent;
    }
    const WorkspaceLockProbe probe = probeWorkspaceLock(record->canonicalPath);
    return probe.held ? HostLiveness::Live : HostLiveness::Stale;
}

WorkspaceRecord WorkspaceRegistry::registerWorkspace(const std::filesystem::path& canonicalPath,
                                                     std::string displayTitle,
                                                     std::optional<std::string> metadata) {
    std::error_code error;
    std::filesystem::path canonical = std::filesystem::canonical(canonicalPath, error);
    if (error) {
        throw_registry(RegistryErrorCode::OpenFailed,
                       "cannot canonicalize workspace path: " + canonicalPath.string());
    }
    if (!std::filesystem::is_directory(canonical, error)) {
        throw_registry(RegistryErrorCode::OpenFailed,
                       "workspace path is not a directory: " + canonical.string());
    }

    Impl::LockGuard guard(*impl_);

    Statement existing{
        impl_->db,
        (std::string{kSelectWorkspaceColumns} + " WHERE canonical_path = ?").c_str()};
    existing.bindText(1, canonical.string());
    if (existing.step() == SQLITE_ROW) {
        return read_workspace(existing);
    }

    if (displayTitle.empty()) {
        displayTitle = basename_title(canonical);
    }
    const WorkspaceId id{generate_uuid_v4()};
    const std::int64_t timestamp = now_ms();

    Transaction transaction{impl_->db};
    Statement insert{
        impl_->db,
        "INSERT INTO workspaces(id, canonical_path, display_title, created_at, updated_at, "
        "metadata) VALUES (?, ?, ?, ?, ?, ?)"};
    insert.bindText(1, id.value);
    insert.bindText(2, canonical.string());
    insert.bindText(3, displayTitle);
    insert.bindInt64(4, timestamp);
    insert.bindInt64(5, timestamp);
    insert.bindOptionalText(6, metadata);
    insert.step();
    transaction.commit();

    WorkspaceRecord record;
    record.id = id;
    record.canonicalPath = canonical;
    record.displayTitle = std::move(displayTitle);
    record.createdAt = timestamp;
    record.updatedAt = timestamp;
    record.metadata = std::move(metadata);
    return record;
}

void WorkspaceRegistry::removeWorkspace(WorkspaceId workspace) {
    Impl::LockGuard guard(*impl_);
    const auto record = findById(workspace);
    if (!record.has_value()) {
        throw_registry(RegistryErrorCode::UnknownWorkspace,
                       "no workspace row for id " + workspace.value);
    }
    {
        Statement count{impl_->db,
                        "SELECT COUNT(*) FROM workspace_sessions WHERE workspace_id = ?"};
        count.bindText(1, workspace.value);
        if (count.step() == SQLITE_ROW && count.columnInt64(0) != 0) {
            throw_registry(RegistryErrorCode::WorkspaceNotEmpty,
                           "workspace still has session junction rows: " + workspace.value);
        }
    }
    if (record->host.has_value() && probeWorkspaceLock(record->canonicalPath).held) {
        throw_registry(RegistryErrorCode::HostClaimed,
                       "workspace has a live host claim: " + workspace.value);
    }
    Transaction transaction{impl_->db};
    delete_marker(impl_->db, workspace);
    Statement statement{impl_->db, "DELETE FROM workspaces WHERE id = ?"};
    statement.bindText(1, workspace.value);
    statement.step();
    transaction.commit();
}

void WorkspaceRegistry::setDisplayTitle(WorkspaceId workspace, std::string displayTitle) {
    Impl::LockGuard guard(*impl_);
    Transaction transaction{impl_->db};
    Statement statement{
        impl_->db, "UPDATE workspaces SET display_title = ?, updated_at = ? WHERE id = ?"};
    statement.bindText(1, displayTitle);
    statement.bindInt64(2, now_ms());
    statement.bindText(3, workspace.value);
    statement.step();
    if (sqlite3_changes(impl_->db) == 0) {
        throw_registry(RegistryErrorCode::UnknownWorkspace,
                       "no workspace row for id " + workspace.value);
    }
    transaction.commit();
}

void WorkspaceRegistry::claimHost(HostClaim claim) {
    if (claim.pid <= 0) {
        throw_registry(RegistryErrorCode::OpenFailed, "host pid must be > 0");
    }
    if (claim.bootId.value.empty()) {
        throw_registry(RegistryErrorCode::OpenFailed, "host boot id must not be empty");
    }
    if (claim.socketPath.empty()) {
        throw_registry(RegistryErrorCode::OpenFailed, "host socket path must not be empty");
    }

    Impl::LockGuard guard(*impl_);
    ensure_workspace_exists(impl_->db, claim.workspace);

    Transaction transaction{impl_->db};
    Statement statement{
        impl_->db,
        "UPDATE workspaces SET host_pid = ?, host_boot_id = ?, host_socket = ?, host_heartbeat = ? "
        "WHERE id = ?"};
    statement.bindInt64(1, claim.pid);
    statement.bindText(2, claim.bootId.value);
    statement.bindText(3, claim.socketPath.string());
    statement.bindInt64(4, now_ms());
    statement.bindText(5, claim.workspace.value);
    statement.step();
    transaction.commit();
}

void WorkspaceRegistry::heartbeat(HostClaim claim) {
    if (claim.pid <= 0 || claim.bootId.value.empty()) {
        return;
    }
    Impl::LockGuard guard(*impl_);
    Transaction transaction{impl_->db};
    Statement statement{
        impl_->db,
        "UPDATE workspaces SET host_heartbeat = ? WHERE id = ? AND host_pid = ? AND "
        "host_boot_id = ?"};
    statement.bindInt64(1, now_ms());
    statement.bindText(2, claim.workspace.value);
    statement.bindInt64(3, claim.pid);
    statement.bindText(4, claim.bootId.value);
    statement.step();
    transaction.commit();
}

void WorkspaceRegistry::releaseHost(WorkspaceId workspace, HostBootId bootId) {
    Impl::LockGuard guard(*impl_);
    Transaction transaction{impl_->db};
    Statement statement{
        impl_->db,
        "UPDATE workspaces SET host_pid = NULL, host_boot_id = NULL, host_socket = NULL, "
        "host_heartbeat = NULL WHERE id = ? AND host_boot_id = ?"};
    statement.bindText(1, workspace.value);
    statement.bindText(2, bootId.value);
    statement.step();
    transaction.commit();
}

bool WorkspaceRegistry::reapHost(WorkspaceId workspace) {
    Impl::LockGuard guard(*impl_);
    const auto record = findById(workspace);
    if (!record.has_value() || !record->host.has_value()) {
        return false;
    }
    if (probeWorkspaceLock(record->canonicalPath).held) {
        return false;
    }
    Transaction transaction{impl_->db};
    Statement statement{
        impl_->db,
        "UPDATE workspaces SET host_pid = NULL, host_boot_id = NULL, host_socket = NULL, "
        "host_heartbeat = NULL WHERE id = ? AND host_pid = ? AND host_boot_id = ?"};
    statement.bindText(1, workspace.value);
    statement.bindInt64(2, record->host->pid);
    statement.bindText(3, record->host->bootId.value);
    statement.step();
    const bool cleared = sqlite3_changes(impl_->db) > 0;
    transaction.commit();
    return cleared;
}

WorkspaceSessionRecord WorkspaceRegistry::addSession(WorkspaceId workspace, SessionId session) {
    Impl::LockGuard guard(*impl_);
    ensure_workspace_exists(impl_->db, workspace);
    if (marker_exists(impl_->db, workspace)) {
        throw_registry(RegistryErrorCode::MutationInProgress,
                       "a pending_mutation marker already exists for " + workspace.value);
    }
    if (const auto existing = findSession(workspace, session); existing.has_value()) {
        return *existing;
    }

    std::int64_t ordinal = 0;
    {
        Statement max{impl_->db,
                      "SELECT COALESCE(MAX(ordinal) + 1, 0) FROM workspace_sessions WHERE "
                      "workspace_id = ?"};
        max.bindText(1, workspace.value);
        if (max.step() == SQLITE_ROW) {
            ordinal = max.columnInt64(0);
        }
    }

    const nlohmann::json payload{{"workspace_id", workspace.value},
                                 {"session_id", session.value},
                                 {"new_ordinal", ordinal},
                                 {"prev_ordinals", prev_ordinals_locked(impl_->db, workspace)}};
    insert_marker(impl_->db, workspace, MutationType::Create, payload.dump());

    Transaction transaction{impl_->db};
    Statement insert{
        impl_->db,
        "INSERT INTO workspace_sessions(workspace_id, session_id, ordinal, archived, created_at) "
        "VALUES (?, ?, ?, 0, ?)"};
    insert.bindText(1, workspace.value);
    insert.bindText(2, session.value);
    insert.bindInt64(3, ordinal);
    insert.bindInt64(4, now_ms());
    insert.step();
    touch_workspace(impl_->db, workspace);
    delete_marker(impl_->db, workspace);
    transaction.commit();

    WorkspaceSessionRecord record;
    record.workspace = workspace;
    record.sessionId = session;
    record.ordinal = ordinal;
    record.archived = false;
    record.createdAt = now_ms();
    return record;
}

void WorkspaceRegistry::removeSession(WorkspaceId workspace, SessionId session) {
    Impl::LockGuard guard(*impl_);
    ensure_workspace_exists(impl_->db, workspace);
    if (marker_exists(impl_->db, workspace)) {
        throw_registry(RegistryErrorCode::MutationInProgress,
                       "a pending_mutation marker already exists for " + workspace.value);
    }
    const nlohmann::json payload{{"workspace_id", workspace.value},
                                 {"session_id", session.value},
                                 {"prev_ordinals", prev_ordinals_locked(impl_->db, workspace)}};
    insert_marker(impl_->db, workspace, MutationType::Delete, payload.dump());

    Transaction transaction{impl_->db};
    Statement statement{impl_->db,
                        "DELETE FROM workspace_sessions WHERE workspace_id = ? AND session_id = ?"};
    statement.bindText(1, workspace.value);
    statement.bindText(2, session.value);
    statement.step();
    touch_workspace(impl_->db, workspace);
    delete_marker(impl_->db, workspace);
    transaction.commit();
}

void WorkspaceRegistry::archiveSession(WorkspaceId workspace, SessionId session, bool archived) {
    Impl::LockGuard guard(*impl_);
    Transaction transaction{impl_->db};
    Statement statement{
        impl_->db,
        "UPDATE workspace_sessions SET archived = ? WHERE workspace_id = ? AND session_id = ?"};
    statement.bindInt64(1, archived ? 1 : 0);
    statement.bindText(2, workspace.value);
    statement.bindText(3, session.value);
    statement.step();
    if (sqlite3_changes(impl_->db) == 0) {
        throw_registry(RegistryErrorCode::UnknownWorkspace,
                       "no junction row for session " + session.value);
    }
    touch_workspace(impl_->db, workspace);
    transaction.commit();
}

void WorkspaceRegistry::reorderSession(WorkspaceId workspace, SessionId session,
                                       std::int64_t newOrdinal) {
    Impl::LockGuard guard(*impl_);
    if (marker_exists(impl_->db, workspace)) {
        throw_registry(RegistryErrorCode::MutationInProgress,
                       "a pending_mutation marker already exists for " + workspace.value);
    }
    std::int64_t fromOrdinal = 0;
    {
        Statement current{
            impl_->db,
            "SELECT ordinal FROM workspace_sessions WHERE workspace_id = ? AND session_id = ?"};
        current.bindText(1, workspace.value);
        current.bindText(2, session.value);
        if (current.step() != SQLITE_ROW) {
            throw_registry(RegistryErrorCode::UnknownWorkspace,
                           "no junction row for session " + session.value);
        }
        fromOrdinal = current.columnInt64(0);
    }

    const nlohmann::json payload{{"workspace_id", workspace.value},
                                 {"session_id", session.value},
                                 {"from_ordinal", fromOrdinal},
                                 {"to_ordinal", newOrdinal},
                                 {"prev_ordinals", prev_ordinals_locked(impl_->db, workspace)}};
    insert_marker(impl_->db, workspace, MutationType::Reorder, payload.dump());

    Transaction transaction{impl_->db};
    apply_reorder(impl_->db, workspace, session.value, fromOrdinal, newOrdinal);
    touch_workspace(impl_->db, workspace);
    delete_marker(impl_->db, workspace);
    transaction.commit();
}

bool WorkspaceRegistry::holdsWriteLock() const { return impl_->write_lock_held; }

void WorkspaceRegistry::resolvePendingMutations() {
    Impl::LockGuard guard(*impl_);

    struct MarkerRow {
        WorkspaceId  workspace;
        MutationType type;
        std::string  payload;
    };
    std::vector<MarkerRow> markers;
    {
        Statement statement{
            impl_->db,
            "SELECT workspace_id, mutation_type, payload FROM pending_mutation ORDER BY "
            "workspace_id"};
        while (statement.step() == SQLITE_ROW) {
            const auto type = parse_mutation_type(statement.columnText(1));
            if (!type.has_value()) {
                throw_registry(RegistryErrorCode::Corrupt,
                               "unknown pending_mutation type for " + statement.columnText(0));
            }
            markers.push_back(MarkerRow{WorkspaceId{statement.columnText(0)}, *type,
                                        statement.columnText(2)});
        }
    }

    for (const auto& marker : markers) {
        nlohmann::json payload;
        try {
            payload = nlohmann::json::parse(marker.payload);
        } catch (const nlohmann::json::exception&) {
            throw_registry(RegistryErrorCode::Corrupt,
                           "unparseable pending_mutation payload for " + marker.workspace.value);
        }
        validate_payload(payload, marker.type, marker.workspace);
        const std::string session = payload.at("session_id").get<std::string>();

        Transaction transaction{impl_->db};
        switch (marker.type) {
            case MutationType::Create: {
                Statement remove{impl_->db,
                                 "DELETE FROM workspace_sessions WHERE workspace_id = ? AND "
                                 "session_id = ?"};
                remove.bindText(1, marker.workspace.value);
                remove.bindText(2, session);
                remove.step();
                restore_prev_ordinals(impl_->db, marker.workspace, payload.at("prev_ordinals"));
                break;
            }
            case MutationType::Delete: {
                Statement remove{impl_->db,
                                 "DELETE FROM workspace_sessions WHERE workspace_id = ? AND "
                                 "session_id = ?"};
                remove.bindText(1, marker.workspace.value);
                remove.bindText(2, session);
                remove.step();
                break;
            }
            case MutationType::Reorder: {
                apply_reorder(impl_->db, marker.workspace, session,
                              payload.at("from_ordinal").get<std::int64_t>(),
                              payload.at("to_ordinal").get<std::int64_t>());
                break;
            }
        }
        delete_marker(impl_->db, marker.workspace);
        transaction.commit();
    }

    Statement mismatch{
        impl_->db,
        "SELECT 1 FROM workspace_sessions GROUP BY workspace_id, ordinal HAVING COUNT(*) > 1"};
    if (mismatch.step() == SQLITE_ROW) {
        throw_registry(RegistryErrorCode::Corrupt,
                       "unmarked duplicate ordinals in workspace_sessions");
    }
}

void WorkspaceRegistry::bootstrap() {
    Impl::LockGuard guard(*impl_);
    if (isInitialized()) {
        return;
    }

    std::vector<std::filesystem::path> roots = impl_->config.workspace_roots;
    if (roots.empty()) {
        roots.push_back(expand_path(std::filesystem::path{"$HOME/prjs"}));
    }

    std::function<void(const std::filesystem::path&, int)> visit =
        [&](const std::filesystem::path& directory, int depth) {
            if (depth > impl_->config.bootstrap_depth) {
                return;
            }
            std::error_code error;
            const std::filesystem::path sessions_db = directory / ".ymh" / "sessions.db";
            if (std::filesystem::is_regular_file(sessions_db, error)) {
                std::filesystem::path canonical = std::filesystem::canonical(directory, error);
                if (!error) {
                    const auto existing = findByCanonicalPath(canonical);
                    WorkspaceId id;
                    if (existing.has_value()) {
                        id = existing->id;
                    } else {
                        const WorkspaceRecord record =
                            registerWorkspace(canonical, basename_title(canonical), std::nullopt);
                        id = record.id;
                    }
                    importSessionsFromDisk(id, canonical);
                }
            }
            std::filesystem::directory_iterator iterator{directory, error};
            if (error) {
                return;
            }
            for (const auto& entry : iterator) {
                if (!entry.is_directory(error) || entry.is_symlink(error)) {
                    continue;
                }
                if (is_denied(entry.path())) {
                    continue;
                }
                visit(entry.path(), depth + 1);
            }
        };

    for (const auto& root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) {
            continue;
        }
        visit(root, 0);
    }

    Transaction transaction{impl_->db};
    Statement marker{
        impl_->db, "INSERT OR REPLACE INTO registry_meta(key, value) VALUES (?, ?)"};
    marker.bindText(1, std::string{kInitializedMarker});
    marker.bindText(2, std::to_string(now_ms()));
    marker.step();
    transaction.commit();
}

std::size_t WorkspaceRegistry::importSessionsFromDisk(WorkspaceId workspace,
                                                      const std::filesystem::path& workspaceRoot) {
    Impl::LockGuard guard(*impl_);
    ensure_workspace_exists(impl_->db, workspace);

    std::error_code error;
    std::filesystem::path canonical = std::filesystem::canonical(workspaceRoot, error);
    if (error) {
        canonical = workspaceRoot;
    }
    const std::filesystem::path sessions_db = canonical / ".ymh" / "sessions.db";
    if (!std::filesystem::is_regular_file(sessions_db, error)) {
        return 0;
    }

    sqlite3* source = nullptr;
    if (sqlite3_open_v2(sessions_db.c_str(), &source, SQLITE_OPEN_READONLY, nullptr) !=
        SQLITE_OK) {
        if (source != nullptr) {
            sqlite3_close_v2(source);
        }
        return 0;
    }

    struct SourceSession {
        std::string  id;
        std::int64_t createdAt = 0;
    };
    std::vector<SourceSession> source_sessions;
    try {
        if (read_pragma_int(source, "PRAGMA application_id") != 0x594D4801) {
            sqlite3_close_v2(source);
            return 0;
        }
        Statement statement{
            source,
            "SELECT id, created_at FROM sessions ORDER BY created_at DESC, updated_at DESC, "
            "id ASC"};
        while (statement.step() == SQLITE_ROW) {
            source_sessions.push_back(
                SourceSession{statement.columnText(0), statement.columnInt64(1)});
        }
    } catch (const RegistryError&) {
        sqlite3_close_v2(source);
        return 0;
    }
    sqlite3_close_v2(source);

    std::int64_t next_ordinal = 0;
    {
        Statement max{impl_->db,
                      "SELECT COALESCE(MAX(ordinal) + 1, 0) FROM workspace_sessions WHERE "
                      "workspace_id = ?"};
        max.bindText(1, workspace.value);
        if (max.step() == SQLITE_ROW) {
            next_ordinal = max.columnInt64(0);
        }
    }

    std::size_t imported = 0;
    Transaction transaction{impl_->db};
    for (const auto& session : source_sessions) {
        Statement exists{
            impl_->db,
            "SELECT 1 FROM workspace_sessions WHERE workspace_id = ? AND session_id = ?"};
        exists.bindText(1, workspace.value);
        exists.bindText(2, session.id);
        if (exists.step() == SQLITE_ROW) {
            continue;
        }
        Statement insert{
            impl_->db,
            "INSERT INTO workspace_sessions(workspace_id, session_id, ordinal, archived, "
            "created_at) VALUES (?, ?, ?, 0, ?)"};
        insert.bindText(1, workspace.value);
        insert.bindText(2, session.id);
        insert.bindInt64(3, next_ordinal);
        insert.bindInt64(4, session.createdAt);
        insert.step();
        ++next_ordinal;
        ++imported;
    }
    transaction.commit();
    return imported;
}

} // namespace ymh

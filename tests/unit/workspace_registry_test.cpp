#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <sqlite3.h>

#include "ymh/registry/liveness.hpp"
#include "ymh/registry/process_scan.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/registry/workspace_cli.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

class TempDir {
public:
    explicit TempDir(const std::string& prefix) {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                (prefix + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

RegistryConfig test_config(const fs::path& dir) {
    RegistryConfig config;
    config.db_path = dir / "registry.db";
    config.lock_path = dir / "registry.lock";
    config.lock_retry_budget = std::chrono::milliseconds{80};
    config.lock_retry_interval = std::chrono::milliseconds{5};
    config.workspace_roots = {dir / "no-such-root"};
    config.bootstrap_depth = 4;
    return config;
}

class RawDb {
public:
    RawDb(const fs::path& path, int flags = SQLITE_OPEN_READWRITE) {
        if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            db_ = nullptr;
        }
    }

    ~RawDb() {
        if (db_ != nullptr) {
            sqlite3_close_v2(db_);
        }
    }

    RawDb(const RawDb&) = delete;
    RawDb& operator=(const RawDb&) = delete;

    [[nodiscard]] bool ok() const { return db_ != nullptr; }

    int exec(const std::string& sql) {
        return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    std::int64_t scalar(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            return -1;
        }
        std::int64_t value = -1;
        if (sqlite3_step(statement) == SQLITE_ROW) {
            value = sqlite3_column_int64(statement, 0);
        }
        sqlite3_finalize(statement);
        return value;
    }

    std::string text(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            return {};
        }
        std::string value;
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char* column = sqlite3_column_text(statement, 0);
            if (column != nullptr) {
                value = reinterpret_cast<const char*>(column);
            }
        }
        sqlite3_finalize(statement);
        return value;
    }

private:
    sqlite3* db_ = nullptr;
};

int hold_file_lock(const fs::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void release_file_lock(int fd) {
    if (fd >= 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
}

void create_sessions_db(const fs::path& root,
                        const std::vector<std::pair<std::string, std::int64_t>>& sessions) {
    const fs::path dir = root / ".ymh";
    std::filesystem::create_directories(dir);
    RawDb db(dir / "sessions.db", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    ASSERT_TRUE(db.ok());
    ASSERT_EQ(db.exec("CREATE TABLE sessions(id TEXT PRIMARY KEY, cwd TEXT, created_at INTEGER, "
                      "updated_at INTEGER, title TEXT, model TEXT, server_profile TEXT, kind TEXT, "
                      "parent_session TEXT, seed_length INTEGER, metadata JSON)"),
              SQLITE_OK);
    for (const auto& [id, created] : sessions) {
        ASSERT_EQ(db.exec("INSERT INTO sessions(id, cwd, created_at, updated_at, title, model, "
                          "server_profile, kind) VALUES('" +
                          id + "', '" + root.string() + "', " + std::to_string(created) + ", " +
                          std::to_string(created) + ", 'title', 'model', 'interactive', 'root')"),
                  SQLITE_OK);
    }
    ASSERT_EQ(db.exec("PRAGMA application_id = 0x594D4801"), SQLITE_OK);
    ASSERT_EQ(db.exec("PRAGMA user_version = 1"), SQLITE_OK);
}

void insert_marker(RawDb& db, const std::string& workspace, const std::string& type,
                   const std::string& payload) {
    ASSERT_EQ(db.exec("INSERT INTO pending_mutation(workspace_id, mutation_type, payload, "
                      "timestamp) VALUES('" +
                      workspace + "', '" + type + "', '" + payload + "', 1)"),
              SQLITE_OK);
}

} // namespace

TEST(WorkspaceRegistryTest, FreshOpenAppliesPragmasAndSchemaVersion) {
    TempDir dir{"ymh_reg_open"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    ASSERT_TRUE(registry->isInitialized());
    EXPECT_TRUE(registry->metaValue(kInitializedMarker).has_value());

    RawDb db(config.db_path);
    ASSERT_TRUE(db.ok());
    EXPECT_EQ(db.scalar("PRAGMA application_id"), 0x594D4802);
    EXPECT_EQ(db.scalar("PRAGMA user_version"), 1);
    EXPECT_EQ(db.text("PRAGMA journal_mode"), "wal");
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
                        "('workspaces','workspace_sessions','pending_mutation','registry_meta')"),
              4);
    EXPECT_EQ(db.scalar("SELECT COUNT(*) FROM pragma_table_info('workspaces') WHERE name IN "
                        "('events','leases','snapshots')"),
              0);
}

TEST(WorkspaceRegistryTest, ForeignApplicationIdIsRejected) {
    TempDir dir{"ymh_reg_foreign"};
    const RegistryConfig config = test_config(dir.path());
    {
        RawDb db(config.db_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        ASSERT_TRUE(db.ok());
        ASSERT_EQ(db.exec("CREATE TABLE t(x)"), SQLITE_OK);
        ASSERT_EQ(db.exec("PRAGMA application_id = 0x12345678"), SQLITE_OK);
        ASSERT_EQ(db.exec("PRAGMA user_version = 1"), SQLITE_OK);
    }
    try {
        auto registry = WorkspaceRegistry::open(config);
        FAIL() << "expected SchemaVersion";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::SchemaVersion);
    }
}

TEST(WorkspaceRegistryTest, NewerSchemaVersionIsRejected) {
    TempDir dir{"ymh_reg_newer"};
    const RegistryConfig config = test_config(dir.path());
    {
        RawDb db(config.db_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        ASSERT_TRUE(db.ok());
        ASSERT_EQ(db.exec("PRAGMA application_id = 0x594D4802"), SQLITE_OK);
        ASSERT_EQ(db.exec("PRAGMA user_version = 99"), SQLITE_OK);
    }
    try {
        auto registry = WorkspaceRegistry::open(config);
        FAIL() << "expected SchemaVersion";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::SchemaVersion);
    }
}

TEST(WorkspaceRegistryTest, ReadOnlyOpenMissingFileAndUninitialized) {
    TempDir dir{"ymh_reg_ro"};
    const RegistryConfig config = test_config(dir.path());
    try {
        auto registry = WorkspaceRegistry::openReadOnly(config);
        FAIL() << "expected OpenFailed";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::OpenFailed);
    }
    EXPECT_FALSE(fs::exists(config.db_path));

    {
        auto registry = WorkspaceRegistry::open(config);
        ASSERT_TRUE(registry->isInitialized());
    }
    {
        RawDb db(config.db_path);
        ASSERT_TRUE(db.ok());
        ASSERT_EQ(db.exec("DELETE FROM registry_meta WHERE key = 'initialized'"), SQLITE_OK);
    }
    try {
        auto registry = WorkspaceRegistry::openReadOnly(config);
        FAIL() << "expected Uninitialized";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::Uninitialized);
    }
}

TEST(WorkspaceRegistryTest, HostPidChecksAndUniqueness) {
    TempDir dir{"ymh_reg_schema"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const WorkspaceRecord record = registry->registerWorkspace(dir.path(), "t");
    registry.reset();

    RawDb db(config.db_path);
    ASSERT_TRUE(db.ok());
    ASSERT_EQ(db.exec("PRAGMA foreign_keys = ON"), SQLITE_OK);
    EXPECT_NE(db.exec("INSERT INTO workspace_sessions(workspace_id, session_id, ordinal, archived, "
                      "created_at) VALUES('missing', 's', 0, 0, 1)"),
              SQLITE_OK);
    EXPECT_NE(db.exec("UPDATE workspaces SET host_pid = 0"), SQLITE_OK);
    EXPECT_NE(db.exec("UPDATE workspaces SET host_pid = 123"), SQLITE_OK);
    EXPECT_NE(db.exec("UPDATE workspaces SET host_pid = 123, host_boot_id = 'b'"), SQLITE_OK);
    EXPECT_NE(db.exec("UPDATE workspaces SET host_pid = 123, host_boot_id = 'b', host_socket = "
                      "'/s'"),
              SQLITE_OK);
    EXPECT_EQ(db.exec("UPDATE workspaces SET host_pid = 123, host_boot_id = 'b', host_socket = "
                      "'/s', host_heartbeat = 1"),
              SQLITE_OK);
    EXPECT_NE(db.exec("INSERT INTO workspaces(id, canonical_path, display_title, created_at, "
                      "updated_at) VALUES('other', '" +
                      record.canonicalPath.string() + "', 'dup', 1, 1)"),
              SQLITE_OK);
    EXPECT_NE(db.exec("INSERT INTO pending_mutation(workspace_id, mutation_type, payload, "
                      "timestamp) VALUES('x', 'bogus', '{}', 1)"),
              SQLITE_OK);
}

TEST(WorkspaceRegistryTest, SymlinkCollisionAttachesToOneWorkspace) {
    TempDir dir{"ymh_reg_symlink"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const fs::path real = dir.path() / "real";
    std::filesystem::create_directories(real);
    const fs::path link = dir.path() / "link";
    std::filesystem::create_directory_symlink(real, link);

    const WorkspaceRecord first = registry->registerWorkspace(real, "first");
    const WorkspaceRecord second = registry->registerWorkspace(link, "second");
    EXPECT_EQ(first.id.value, second.id.value);
    EXPECT_EQ(registry->listWorkspaces().size(), 1U);
}

TEST(WorkspaceRegistryTest, AddSessionOrdinalsAreIncreasingAndGapTolerant) {
    TempDir dir{"ymh_reg_sessions"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const WorkspaceRecord record = registry->registerWorkspace(dir.path(), "t");

    const WorkspaceSessionRecord a = registry->addSession(record.id, SessionId{"a"});
    const WorkspaceSessionRecord b = registry->addSession(record.id, SessionId{"b"});
    const WorkspaceSessionRecord c = registry->addSession(record.id, SessionId{"c"});
    EXPECT_EQ(a.ordinal, 0);
    EXPECT_EQ(b.ordinal, 1);
    EXPECT_EQ(c.ordinal, 2);

    registry->removeSession(record.id, SessionId{"b"});
    auto sessions = registry->listSessions(record.id);
    ASSERT_EQ(sessions.size(), 2U);
    EXPECT_EQ(sessions[0].ordinal, 0);
    EXPECT_EQ(sessions[1].ordinal, 2);

    const WorkspaceSessionRecord d = registry->addSession(record.id, SessionId{"d"});
    EXPECT_EQ(d.ordinal, 3);
}

TEST(WorkspaceRegistryTest, OrderSessionsJunctionFirstThenStoreOnlyByCreatedAt) {
    TempDir dir{"ymh_reg_order"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const WorkspaceRecord record = registry->registerWorkspace(dir.path(), "t");

    registry->addSession(record.id, SessionId{"b"});
    registry->addSession(record.id, SessionId{"a"});

    const std::vector<SessionRef> store{
        SessionRef{SessionId{"z"}, 50},
        SessionRef{SessionId{"a"}, 10},
        SessionRef{SessionId{"c"}, 20},
    };
    const std::vector<SessionOrderEntry> ordered =
        registry->listSessionsOrdered(record.id, store);

    ASSERT_EQ(ordered.size(), 4U);
    EXPECT_EQ(ordered[0].sessionId.value, "b");
    EXPECT_EQ(ordered[0].ordinal, 0);
    EXPECT_TRUE(ordered[0].inJunction);
    EXPECT_EQ(ordered[1].sessionId.value, "a");
    EXPECT_EQ(ordered[1].ordinal, 1);
    EXPECT_TRUE(ordered[1].inJunction);
    EXPECT_EQ(ordered[2].sessionId.value, "c");
    EXPECT_FALSE(ordered[2].inJunction);
    EXPECT_EQ(ordered[3].sessionId.value, "z");
    EXPECT_FALSE(ordered[3].inJunction);
}

TEST(WorkspaceRegistryTest, OrderSessionsUsesOrdinalNotLexicalId) {
    const std::vector<WorkspaceSessionRecord> junction{
        WorkspaceSessionRecord{WorkspaceId{"w"}, SessionId{"zzz"}, 1, false, 0},
        WorkspaceSessionRecord{WorkspaceId{"w"}, SessionId{"aaa"}, 0, false, 0},
    };
    const std::vector<SessionOrderEntry> ordered = orderSessions(junction, {});
    ASSERT_EQ(ordered.size(), 2U);
    EXPECT_EQ(ordered[0].sessionId.value, "aaa");
    EXPECT_EQ(ordered[1].sessionId.value, "zzz");
}

TEST(WorkspaceRegistryTest, ReorderWritesMarkerThenClearsIt) {
    TempDir dir{"ymh_reg_reorder"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const WorkspaceRecord record = registry->registerWorkspace(dir.path(), "t");
    registry->addSession(record.id, SessionId{"a"});
    registry->addSession(record.id, SessionId{"b"});
    registry->addSession(record.id, SessionId{"c"});

    registry->reorderSession(record.id, SessionId{"c"}, 0);
    EXPECT_FALSE(registry->pendingMutation(record.id).has_value());

    const auto sessions = registry->listSessions(record.id);
    ASSERT_EQ(sessions.size(), 3U);
    EXPECT_EQ(sessions[0].sessionId.value, "c");
    EXPECT_EQ(sessions[0].ordinal, 0);
    EXPECT_EQ(sessions[1].sessionId.value, "a");
    EXPECT_EQ(sessions[2].sessionId.value, "b");
}

TEST(WorkspaceRegistryTest, MutationInProgressWhenMarkerExists) {
    TempDir dir{"ymh_reg_marker"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const WorkspaceRecord record = registry->registerWorkspace(dir.path(), "t");
    registry.reset();

    RawDb db(config.db_path);
    ASSERT_TRUE(db.ok());
    insert_marker(db, record.id.value, "create", "{}");

    auto reopened = WorkspaceRegistry::openReadOnly(config);
    EXPECT_TRUE(reopened->pendingMutation(record.id).has_value());
    EXPECT_THROW(reopened->addSession(record.id, SessionId{"a"}), RegistryError);
}

TEST(WorkspaceRegistryTest, ClaimHeartbeatReleaseAndLiveness) {
    TempDir dir{"ymh_reg_claim"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const fs::path workspace = dir.path() / "ws";
    std::filesystem::create_directories(workspace);
    const WorkspaceRecord record = registry->registerWorkspace(workspace, "ws");

    HostClaim claim;
    claim.workspace = record.id;
    claim.pid = 4242;
    claim.bootId = HostBootId{"boot-1"};
    claim.socketPath = workspace / ".ymh" / "host.sock";
    registry->claimHost(claim);

    auto stored = registry->findById(record.id);
    ASSERT_TRUE(stored.has_value());
    ASSERT_TRUE(stored->host.has_value());
    EXPECT_EQ(stored->host->pid, 4242);
    EXPECT_TRUE(stored->heartbeatAt.has_value());
    EXPECT_EQ(registry->liveHostCount(), 1U);
    EXPECT_EQ(registry->probeLiveness(record.id), HostLiveness::Stale);

    {
        RawDb db(config.db_path);
        ASSERT_TRUE(db.ok());
        ASSERT_EQ(db.exec("UPDATE workspaces SET host_heartbeat = 1"), SQLITE_OK);
    }
    HostClaim other = claim;
    other.pid = 9999;
    registry->heartbeat(other);
    stored = registry->findById(record.id);
    EXPECT_EQ(stored->heartbeatAt.value(), 1);
    registry->heartbeat(claim);
    stored = registry->findById(record.id);
    EXPECT_GT(stored->heartbeatAt.value(), 1);

    const int lock_fd = hold_file_lock(workspace / ".ymh" / "sessions.lock");
    ASSERT_GE(lock_fd, 0);
    EXPECT_EQ(registry->probeLiveness(record.id), HostLiveness::Live);
    EXPECT_FALSE(registry->reapHost(record.id));
    EXPECT_TRUE(registry->findById(record.id)->host.has_value());
    release_file_lock(lock_fd);

    EXPECT_TRUE(registry->reapHost(record.id));
    EXPECT_FALSE(registry->findById(record.id)->host.has_value());
    EXPECT_EQ(registry->liveHostCount(), 0U);

    registry->claimHost(claim);
    registry->releaseHost(record.id, HostBootId{"other-boot"});
    EXPECT_TRUE(registry->findById(record.id)->host.has_value());
    registry->releaseHost(record.id, HostBootId{"boot-1"});
    EXPECT_FALSE(registry->findById(record.id)->host.has_value());
}

TEST(WorkspaceRegistryTest, RemoveWorkspaceGuards) {
    TempDir dir{"ymh_reg_remove"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const fs::path workspace = dir.path() / "ws";
    std::filesystem::create_directories(workspace);
    const WorkspaceRecord record = registry->registerWorkspace(workspace, "ws");
    registry->addSession(record.id, SessionId{"a"});
    try {
        registry->removeWorkspace(record.id);
        FAIL() << "expected WorkspaceNotEmpty";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::WorkspaceNotEmpty);
    }
    registry->removeSession(record.id, SessionId{"a"});

    HostClaim claim;
    claim.workspace = record.id;
    claim.pid = 4242;
    claim.bootId = HostBootId{"boot-1"};
    claim.socketPath = workspace / ".ymh" / "host.sock";
    registry->claimHost(claim);
    const int lock_fd = hold_file_lock(workspace / ".ymh" / "sessions.lock");
    ASSERT_GE(lock_fd, 0);
    try {
        registry->removeWorkspace(record.id);
        FAIL() << "expected HostClaimed";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::HostClaimed);
    }
    release_file_lock(lock_fd);

    registry->removeWorkspace(record.id);
    EXPECT_FALSE(registry->findById(record.id).has_value());
}

TEST(WorkspaceRegistryTest, BootstrapHonoursDenylistAndDepth) {
    TempDir dir{"ymh_reg_bootstrap"};
    const fs::path root = dir.path() / "prjs";
    create_sessions_db(root / "ws1", {{"s-old", 100}, {"s-new", 300}, {"s-mid", 200}});
    create_sessions_db(root / "node_modules" / "ws2", {{"x", 1}});
    create_sessions_db(root / "build" / "ws3", {{"x", 1}});
    create_sessions_db(root / "a" / "b" / "c" / "d" / "e" / "ws4", {{"x", 1}});

    RegistryConfig config = test_config(dir.path());
    config.workspace_roots = {root};
    auto registry = WorkspaceRegistry::open(config);

    const auto workspaces = registry->listWorkspaces();
    ASSERT_EQ(workspaces.size(), 1U);
    EXPECT_EQ(workspaces[0].canonicalPath.filename().string(), "ws1");
    EXPECT_EQ(workspaces[0].displayTitle, "ws1");

    const auto sessions = registry->listSessions(workspaces[0].id);
    ASSERT_EQ(sessions.size(), 3U);
    EXPECT_EQ(sessions[0].sessionId.value, "s-new");
    EXPECT_EQ(sessions[0].ordinal, 0);
    EXPECT_EQ(sessions[1].sessionId.value, "s-mid");
    EXPECT_EQ(sessions[2].sessionId.value, "s-old");
    EXPECT_EQ(sessions[2].ordinal, 2);
    EXPECT_FALSE(sessions[0].archived);
}

TEST(WorkspaceRegistryTest, BootstrapIsIdempotentAndNonexistentRootIsNotAnError) {
    TempDir dir{"ymh_reg_idem"};
    const fs::path root = dir.path() / "prjs";
    create_sessions_db(root / "ws1", {{"s1", 1}});

    RegistryConfig config = test_config(dir.path());
    config.workspace_roots = {root};
    {
        auto registry = WorkspaceRegistry::open(config);
        EXPECT_EQ(registry->listWorkspaces().size(), 1U);
    }
    {
        auto registry = WorkspaceRegistry::open(config);
        EXPECT_EQ(registry->listWorkspaces().size(), 1U);
        EXPECT_EQ(registry->listSessions(registry->listWorkspaces()[0].id).size(), 1U);
    }

    TempDir other{"ymh_reg_noroot"};
    RegistryConfig missing = test_config(other.path());
    missing.workspace_roots = {other.path() / "absent"};
    auto registry = WorkspaceRegistry::open(missing);
    EXPECT_TRUE(registry->listWorkspaces().empty());
    EXPECT_TRUE(registry->isInitialized());
}

TEST(WorkspaceRegistryTest, ImportSessionsFromDiskIsNewestFirstAndIdempotent) {
    TempDir dir{"ymh_reg_import"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const fs::path workspace = dir.path() / "ws";
    create_sessions_db(workspace, {{"s-old", 10}, {"s-new", 30}, {"s-mid", 20}});
    const WorkspaceRecord record = registry->registerWorkspace(workspace, "ws");

    EXPECT_EQ(registry->importSessionsFromDisk(record.id, workspace), 3U);
    EXPECT_EQ(registry->importSessionsFromDisk(record.id, workspace), 0U);
    const auto sessions = registry->listSessions(record.id);
    ASSERT_EQ(sessions.size(), 3U);
    EXPECT_EQ(sessions[0].sessionId.value, "s-new");
    EXPECT_EQ(sessions[1].sessionId.value, "s-mid");
    EXPECT_EQ(sessions[2].sessionId.value, "s-old");
}

TEST(WorkspaceRegistryTest, PendingCreateRollsBackDeleteCompletesReorderApplies) {
    TempDir dir{"ymh_reg_pending"};
    const RegistryConfig config = test_config(dir.path());
    auto registry = WorkspaceRegistry::open(config);
    const WorkspaceRecord record = registry->registerWorkspace(dir.path(), "t");
    registry->addSession(record.id, SessionId{"a"});
    registry->addSession(record.id, SessionId{"b"});
    registry.reset();

    {
        RawDb db(config.db_path);
        ASSERT_TRUE(db.ok());
        ASSERT_EQ(db.exec("INSERT INTO workspace_sessions(workspace_id, session_id, ordinal, "
                          "archived, created_at) VALUES('" +
                          record.id.value + "', 'c', 2, 0, 1)"),
                  SQLITE_OK);
        insert_marker(db, record.id.value, "create",
                      R"({"workspace_id":")" + record.id.value +
                          R"(","session_id":"c","new_ordinal":2,"prev_ordinals":{"a":0,"b":1}})");
    }
    {
        auto reopened = WorkspaceRegistry::open(config);
        const auto sessions = reopened->listSessions(record.id);
        ASSERT_EQ(sessions.size(), 2U);
        EXPECT_EQ(sessions[0].sessionId.value, "a");
        EXPECT_EQ(sessions[0].ordinal, 0);
        EXPECT_EQ(sessions[1].sessionId.value, "b");
        EXPECT_FALSE(reopened->pendingMutation(record.id).has_value());
    }

    {
        RawDb db(config.db_path);
        ASSERT_TRUE(db.ok());
        insert_marker(db, record.id.value, "delete",
                      R"({"workspace_id":")" + record.id.value +
                          R"(","session_id":"b","prev_ordinals":{"a":0,"b":1}})");
    }
    {
        auto reopened = WorkspaceRegistry::open(config);
        const auto sessions = reopened->listSessions(record.id);
        ASSERT_EQ(sessions.size(), 1U);
        EXPECT_EQ(sessions[0].sessionId.value, "a");
    }

    {
        RawDb db(config.db_path);
        ASSERT_TRUE(db.ok());
        ASSERT_EQ(db.exec("INSERT INTO workspace_sessions(workspace_id, session_id, ordinal, "
                          "archived, created_at) VALUES('" +
                          record.id.value + "', 'c', 2, 0, 1)"),
                  SQLITE_OK);
        insert_marker(db, record.id.value, "reorder",
                      R"({"workspace_id":")" + record.id.value +
                          R"(","session_id":"c","from_ordinal":2,"to_ordinal":0,)"
                          R"("prev_ordinals":{"a":0,"c":2}})");
    }
    {
        auto reopened = WorkspaceRegistry::open(config);
        const auto sessions = reopened->listSessions(record.id);
        ASSERT_EQ(sessions.size(), 2U);
        EXPECT_EQ(sessions[0].sessionId.value, "c");
        EXPECT_EQ(sessions[0].ordinal, 0);
        EXPECT_EQ(sessions[1].sessionId.value, "a");
        EXPECT_EQ(sessions[1].ordinal, 1);
    }
}

TEST(WorkspaceRegistryTest, MalformedMarkerAndUnmarkedMismatchFailLoud) {
    TempDir dir{"ymh_reg_corrupt"};
    const RegistryConfig config = test_config(dir.path());
    {
        auto registry = WorkspaceRegistry::open(config);
        const WorkspaceRecord record = registry->registerWorkspace(dir.path(), "t");
        registry->addSession(record.id, SessionId{"a"});
        registry->addSession(record.id, SessionId{"b"});
    }
    {
        RawDb db(config.db_path);
        ASSERT_TRUE(db.ok());
        const std::string workspace =
            db.text("SELECT id FROM workspaces LIMIT 1");
        insert_marker(db, workspace, "create", R"({"session_id": 5})");
    }
    try {
        auto registry = WorkspaceRegistry::open(config);
        FAIL() << "expected Corrupt";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::Corrupt);
    }

    TempDir second{"ymh_reg_mismatch"};
    const RegistryConfig other = test_config(second.path());
    {
        auto registry = WorkspaceRegistry::open(other);
        const WorkspaceRecord record = registry->registerWorkspace(second.path(), "t");
        registry->addSession(record.id, SessionId{"a"});
        registry->addSession(record.id, SessionId{"b"});
    }
    {
        RawDb db(other.db_path);
        ASSERT_TRUE(db.ok());
        ASSERT_EQ(db.exec("UPDATE workspace_sessions SET ordinal = 0"), SQLITE_OK);
    }
    try {
        auto registry = WorkspaceRegistry::open(other);
        FAIL() << "expected Corrupt";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::Corrupt);
    }
}

TEST(WorkspaceRegistryTest, WriterContentionAndLockFreeReaders) {
    TempDir dir{"ymh_reg_contention"};
    const RegistryConfig config = test_config(dir.path());
    auto writer = WorkspaceRegistry::open(config);

    const int lock_fd = hold_file_lock(config.lock_path);
    ASSERT_GE(lock_fd, 0);

    auto reader = WorkspaceRegistry::openReadOnly(config);
    EXPECT_NO_THROW((void)reader->listWorkspaces());

    try {
        writer->registerWorkspace(dir.path(), "t");
        FAIL() << "expected LockUnavailable";
    } catch (const RegistryError& error) {
        EXPECT_EQ(error.code(), RegistryErrorCode::LockUnavailable);
    }

    release_file_lock(lock_fd);
    EXPECT_NO_THROW(writer->registerWorkspace(dir.path(), "t"));
    EXPECT_EQ(writer->listWorkspaces().size(), 1U);
}

TEST(WorkspaceRegistryTest, ProcessScanSeedsOnlyAndNeverTrustsArgv) {
    EXPECT_TRUE(is_ymh_host_process(
        {"ymh", "--host", "--workspace", "u", "--socket", "/s"}));
    EXPECT_TRUE(is_ymh_host_process(
        {"/usr/local/bin/ymh", "--host", "--workspace", "u", "--socket", "/s"}));
    EXPECT_FALSE(is_ymh_host_process({"ymhx", "--host", "--workspace", "u", "--socket", "/s"}));
    EXPECT_FALSE(is_ymh_host_process({"ymh", "--workspace", "u", "--socket", "/s"}));

    const fs::path root = fs::temp_directory_path() / "ymh_reg_scan_root";
    ProcessEntry entry;
    entry.pid = 42;
    entry.argv = {"ymh", "--host", "--workspace", "11111111-1111-4111-8111-111111111111",
                  "--socket", "/tmp/spoofed.sock"};
    entry.cwd = root;
    const std::vector<ProcessEntry> table{entry};

    WorkspaceRecord record;
    record.id = WorkspaceId{"11111111-1111-4111-8111-111111111111"};
    record.canonicalPath = root;
    const auto lookup = [&](const WorkspaceId& id) -> std::optional<WorkspaceRecord> {
        return id.value == record.id.value ? std::optional<WorkspaceRecord>{record} : std::nullopt;
    };

    int confirmations = 0;
    const auto confirm = [&](const fs::path& workspace_root, const fs::path& socket) {
        ++confirmations;
        return socket == workspace_root / ".ymh" / "host.sock";
    };
    const auto hosts = scanHosts(table, lookup, confirm);
    ASSERT_EQ(hosts.size(), 1U);
    EXPECT_EQ(confirmations, 1);
    EXPECT_EQ(hosts[0].pid, 42);
    EXPECT_EQ(hosts[0].workspaceRoot, root);
    EXPECT_EQ(hosts[0].socketPath, root / ".ymh" / "host.sock");
    EXPECT_NE(hosts[0].socketPath, fs::path{"/tmp/spoofed.sock"});

    const auto reject = [](const fs::path&, const fs::path&) { return false; };
    EXPECT_TRUE(scanHosts(table, lookup, reject).empty());
}

TEST(WorkspaceRegistryTest, WorkspaceCliAddAndList) {
    TempDir state{"ymh_reg_cli_state"};
    TempDir workspace{"ymh_reg_cli_ws"};
    create_sessions_db(workspace.path(), {{"s1", 1}});

    const std::string previous =
        std::getenv("XDG_STATE_HOME") != nullptr ? std::getenv("XDG_STATE_HOME") : std::string{};
    ASSERT_EQ(::setenv("XDG_STATE_HOME", state.path().c_str(), 1), 0);

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(run_workspace_command({"add", workspace.path().string()}, out, err), 0);
    EXPECT_NE(out.str().find(workspace.path().string()), std::string::npos);

    out.str({});
    out.clear();
    EXPECT_EQ(run_workspace_command({"list"}, out, err), 0);
    EXPECT_NE(out.str().find(workspace.path().string()), std::string::npos);

    if (previous.empty()) {
        ::unsetenv("XDG_STATE_HOME");
    } else {
        ::setenv("XDG_STATE_HOME", previous.c_str(), 1);
    }
}

TEST(WorkspaceRegistryTest, StateDirScaffoldingRespectsXdg) {
    TempDir dir{"ymh_reg_state"};
    const std::string previous = std::getenv("XDG_STATE_HOME") != nullptr
                                     ? std::getenv("XDG_STATE_HOME")
                                     : std::string{};
    ASSERT_EQ(::setenv("XDG_STATE_HOME", dir.path().c_str(), 1), 0);
    EXPECT_EQ(default_state_dir(), dir.path() / "ymh");
    EXPECT_EQ(default_registry_db_path(), dir.path() / "ymh" / "registry.db");
    EXPECT_EQ(default_registry_lock_path(), dir.path() / "ymh" / "registry.lock");
    if (previous.empty()) {
        ::unsetenv("XDG_STATE_HOME");
    } else {
        ::setenv("XDG_STATE_HOME", previous.c_str(), 1);
    }
}

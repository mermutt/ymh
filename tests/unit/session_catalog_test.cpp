#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <sqlite3.h>

#include "ymh/core/event.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/ui/session_catalog.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

std::filesystem::path make_temp_dir(const std::string& prefix) {
    const auto base = std::filesystem::temp_directory_path() /
                      (prefix + "_" + std::to_string(::getpid()) + "_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

class TempDir {
public:
    explicit TempDir(const std::string& prefix) : path_(make_temp_dir(prefix)) {}

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class ScopedPermissions {
public:
    ScopedPermissions(std::filesystem::path path, std::filesystem::perms perms)
        : path_(std::move(path)) {
        std::filesystem::permissions(path_, perms, std::filesystem::perm_options::replace);
    }

    ~ScopedPermissions() {
        std::error_code error;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
    }

    ScopedPermissions(const ScopedPermissions&) = delete;
    ScopedPermissions& operator=(const ScopedPermissions&) = delete;

private:
    std::filesystem::path path_;
};

class RawDb {
public:
    explicit RawDb(const std::filesystem::path& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            if (db_ != nullptr) {
                sqlite3_close_v2(db_);
                db_ = nullptr;
            }
            throw std::runtime_error("raw open failed: " + path.string());
        }
    }

    ~RawDb() {
        if (db_ != nullptr) {
            sqlite3_close_v2(db_);
        }
    }

    RawDb(const RawDb&) = delete;
    RawDb& operator=(const RawDb&) = delete;

    void exec(const std::string& sql) {
        sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

private:
    sqlite3* db_ = nullptr;
};

int dump_row(void* context, int count, char** values, char** names) {
    auto* out = static_cast<std::string*>(context);
    for (int i = 0; i < count; ++i) {
        *out += names[i] != nullptr ? names[i] : "";
        *out += '=';
        *out += values[i] != nullptr ? values[i] : "NULL";
        *out += '|';
    }
    *out += '\n';
    return 0;
}

std::string dump_rows(const std::filesystem::path& db_path, const std::string& table) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        if (db != nullptr) {
            sqlite3_close_v2(db);
        }
        return {};
    }
    std::string out;
    const std::string sql = "SELECT * FROM " + table + " ORDER BY 1";
    sqlite3_exec(db, sql.c_str(), &dump_row, &out, nullptr);
    sqlite3_close_v2(db);
    return out;
}

std::filesystem::path db_path_of(const std::filesystem::path& root) {
    return root / ".ymh" / "sessions.db";
}

PersistenceConfig config_for(const std::filesystem::path& root) {
    PersistenceConfig config;
    config.db_path   = db_path_of(root);
    config.lock_path = root / ".ymh" / "sessions.lock";
    config.boot_id   = BootId{"catalog-test"};
    return config;
}

void remove_sidecars(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove(root / ".ymh" / "sessions.db-wal", error);
    std::filesystem::remove(root / ".ymh" / "sessions.db-shm", error);
}

SessionHeader make_header(const std::filesystem::path& root, std::int64_t created,
                          std::int64_t updated, std::string title, std::string model) {
    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = std::filesystem::canonical(root);
    header.createdAt     = created;
    header.updatedAt     = updated;
    header.title         = std::move(title);
    header.model         = std::move(model);
    header.serverProfile = "interactive";
    header.kind          = SessionKind::Root;
    return header;
}

void create_valid_db(const std::filesystem::path& root, std::vector<SessionHeader> headers) {
    const std::unique_ptr<SessionPersistence> store = SessionPersistence::open(config_for(root));
    for (SessionHeader& header : headers) {
        store->create(std::move(header));
    }
    remove_sidecars(root);
}

void write_text_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
}

void set_pragma(const std::filesystem::path& root, const std::string& sql) {
    RawDb db(db_path_of(root));
    db.exec(sql);
    remove_sidecars(root);
}

void corrupt_with_orphan_event(const std::filesystem::path& root) {
    RawDb db(db_path_of(root));
    db.exec("PRAGMA foreign_keys = OFF");
    db.exec("INSERT INTO events(sequence, session_id, event_id, timestamp, type, payload) "
            "VALUES (1, 'orphan-session', 'evt-orphan', 0, 'session_started', '{}')");
    remove_sidecars(root);
}

WorkspaceRecord record_for(const std::filesystem::path& root, const std::string& title) {
    WorkspaceRecord record;
    record.id            = WorkspaceId{"ws-" + root.filename().string()};
    record.canonicalPath = root;
    record.displayTitle  = title;
    return record;
}

WorkspaceRecord record_with_id(const std::string& id, const std::string& title) {
    WorkspaceRecord record;
    record.id            = WorkspaceId{id};
    record.canonicalPath = std::filesystem::path{"/nonexistent"} / id;
    record.displayTitle  = title;
    return record;
}

WorkspaceHistory make_history(const WorkspaceRecord& record, bool live) {
    WorkspaceHistory history;
    history.id            = record.id;
    history.title         = record.displayTitle;
    history.canonicalPath = record.canonicalPath.string();
    history.live          = live;
    SessionHistoryEntry entry;
    entry.id        = SessionId{"sess-" + record.id.value};
    entry.title     = "t";
    entry.kind      = "root";
    entry.updatedAt = 42;
    history.sessions.push_back(entry);
    return history;
}

class SnapshotCollector {
public:
    void push(SessionCatalogSnapshot snapshot) {
        {
            const std::lock_guard lock(mutex_);
            snapshots_.push_back(std::move(snapshot));
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool wait_for_count(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return snapshots_.size() >= count; });
    }

    [[nodiscard]] SessionCatalogSnapshot at(std::size_t index) {
        const std::lock_guard lock(mutex_);
        return snapshots_.at(index);
    }

    [[nodiscard]] std::size_t count() {
        const std::lock_guard lock(mutex_);
        return snapshots_.size();
    }

private:
    std::mutex                          mutex_;
    std::condition_variable             cv_;
    std::vector<SessionCatalogSnapshot> snapshots_;
};

WorkspaceCatalogSource single_source(std::function<WorkspaceHistory(const WorkspaceRecord&, bool)> read) {
    WorkspaceCatalogSource source;
    source.list    = [] { return std::vector<WorkspaceRecord>{record_with_id("w1", "one")}; };
    source.is_live = [](const WorkspaceId&) { return false; };
    source.read    = std::move(read);
    return source;
}

} // namespace

TEST(SessionCatalogRead, SW_U4_MapsSessionHeaderAndSortsUpdatedDescIdAsc) {
    TempDir workspace("ymh_catalog_map");
    std::vector<SessionHeader> headers;
    headers.push_back(make_header(workspace.path(), 100, 3000, "newest", "model-a"));
    headers.push_back(make_header(workspace.path(), 100, 1000, "oldest", "model-b"));
    headers.push_back(make_header(workspace.path(), 100, 2000, "middle", "model-c"));
    create_valid_db(workspace.path(), std::move(headers));

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_FALSE(history.note.has_value());
    ASSERT_EQ(history.sessions.size(), 3u);
    EXPECT_EQ(history.sessions[0].title, "newest");
    EXPECT_EQ(history.sessions[1].title, "middle");
    EXPECT_EQ(history.sessions[2].title, "oldest");
    for (const SessionHistoryEntry& entry : history.sessions) {
        EXPECT_EQ(entry.kind, "root");
        EXPECT_FALSE(entry.title.empty());
        EXPECT_FALSE(entry.model.empty());
        EXPECT_GT(entry.updatedAt, 0);
    }
    EXPECT_EQ(history.canonicalPath, workspace.path().string());
    EXPECT_FALSE(history.live);
}

TEST(SessionCatalogRead, SW_U4_TiesBreakByIdAscending) {
    TempDir workspace("ymh_catalog_tie");
    SessionHeader first  = make_header(workspace.path(), 100, 2000, "a", "m");
    SessionHeader second = make_header(workspace.path(), 100, 2000, "b", "m");
    const std::string low_id  = std::min(first.id.value, second.id.value);
    create_valid_db(workspace.path(), {std::move(first), std::move(second)});

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_FALSE(history.note.has_value());
    ASSERT_EQ(history.sessions.size(), 2u);
    EXPECT_EQ(history.sessions[0].id.value, low_id);
    EXPECT_LT(history.sessions[0].id.value, history.sessions[1].id.value);
}

TEST(SessionCatalogRead, SW_U4_WorkspaceMissing) {
    TempDir parent("ymh_catalog_missing_ws");
    WorkspaceRecord record = record_for(parent.path() / "deleted", "gone");

    const WorkspaceHistory history = read_workspace_history(record, true);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "workspace missing");
    EXPECT_TRUE(history.sessions.empty());
    EXPECT_TRUE(history.live);
}

TEST(SessionCatalogRead, SW_U4_NoSessionsDb) {
    TempDir workspace("ymh_catalog_nodb");
    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "no sessions.db");
}

TEST(SessionCatalogRead, SW_U4_NotAFile) {
    TempDir workspace("ymh_catalog_notfile");
    std::filesystem::create_directories(db_path_of(workspace.path()));

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "not a file");
}

TEST(SessionCatalogRead, SW_U4_Unreadable) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "permission-dependent case; running as root";
    }
    TempDir workspace("ymh_catalog_unreadable");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 100, "t", "m")});
    const ScopedPermissions permissions(db_path_of(workspace.path()), std::filesystem::perms::none);

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "unreadable");
}

TEST(SessionCatalogRead, SW_U4_GarbageIsCorrupt) {
    TempDir workspace("ymh_catalog_garbage");
    write_text_file(db_path_of(workspace.path()), "this is not a sqlite database at all");

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "corrupt");
}

TEST(SessionCatalogRead, SW_U4_ZeroByteIsCorrupt) {
    TempDir workspace("ymh_catalog_zero");
    write_text_file(db_path_of(workspace.path()), "");

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "corrupt");
}

TEST(SessionCatalogRead, SW_U4_ReadOnlyDirectory) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "permission-dependent case; running as root";
    }
    TempDir workspace("ymh_catalog_readonly");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 100, "t", "m")});
    remove_sidecars(workspace.path());
    const ScopedPermissions permissions(
        workspace.path() / ".ymh",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec);

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "read-only location");
    EXPECT_TRUE(history.sessions.empty());
}

TEST(SessionCatalogRead, SW_U4_BrokenForeignKeyIsCorrupt) {
    TempDir workspace("ymh_catalog_fk");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 100, "t", "m")});
    corrupt_with_orphan_event(workspace.path());

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "corrupt");
}

TEST(SessionCatalogRead, SW_U4_ForeignAppIdIsSchemaMismatch) {
    TempDir workspace("ymh_catalog_appid");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 100, "t", "m")});
    set_pragma(workspace.path(), "PRAGMA application_id = 0x12345678");

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "schema mismatch");
}

TEST(SessionCatalogRead, SW_U4_AppIdZeroIsAccepted) {
    TempDir workspace("ymh_catalog_appid0");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 100, "t", "m")});
    set_pragma(workspace.path(), "PRAGMA application_id = 0");

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    EXPECT_FALSE(history.note.has_value());
    EXPECT_EQ(history.sessions.size(), 1u);
}

TEST(SessionCatalogRead, SW_U4_BadUserVersionIsSchemaMismatch) {
    TempDir workspace("ymh_catalog_userversion");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 100, "t", "m")});
    set_pragma(workspace.path(), "PRAGMA user_version = 2");

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "schema mismatch");
}

TEST(SessionCatalogRead, SW_U20_PreflightYieldsExactlyOneNoteAndNeverThrows) {
    TempDir workspace("ymh_catalog_u20");
    std::filesystem::create_directories(workspace.path() / ".ymh");

    const auto classify = [](const WorkspaceRecord& record) {
        WorkspaceHistory history;
        EXPECT_NO_THROW(history = read_workspace_history(record, false));
        return history;
    };

    // Pre-flight 0 precedence: a deleted directory classifies as "workspace
    // missing", not "no sessions.db".
    {
        const WorkspaceHistory history =
            classify(record_for(workspace.path() / "deleted", "gone"));
        ASSERT_TRUE(history.note.has_value());
        EXPECT_EQ(*history.note, "workspace missing");
    }

    // Pre-flight 4: bad magic classifies as "corrupt" (the generic StoreError
    // arm is never reached).
    {
        TempDir garbage("ymh_catalog_u20_garbage");
        write_text_file(db_path_of(garbage.path()), "garbage-bytes-1234567890");
        const WorkspaceHistory history = classify(record_for(garbage.path(), "garbage"));
        ASSERT_TRUE(history.note.has_value());
        EXPECT_EQ(*history.note, "corrupt");
    }

    // Valid magic but a broken FK → "corrupt" from verify_consistency.
    {
        TempDir broken("ymh_catalog_u20_fk");
        create_valid_db(broken.path(), {make_header(broken.path(), 100, 100, "t", "m")});
        corrupt_with_orphan_event(broken.path());
        const WorkspaceHistory history = classify(record_for(broken.path(), "broken"));
        ASSERT_TRUE(history.note.has_value());
        EXPECT_EQ(*history.note, "corrupt");
    }

    // Missing database → "no sessions.db".
    {
        TempDir empty("ymh_catalog_u20_empty");
        const WorkspaceHistory history = classify(record_for(empty.path(), "empty"));
        ASSERT_TRUE(history.note.has_value());
        EXPECT_EQ(*history.note, "no sessions.db");
    }

    // chmod 0000 file → "unreadable" (pre-flight 3); non-root only.
    if (::geteuid() != 0) {
        TempDir unreadable("ymh_catalog_u20_unreadable");
        create_valid_db(unreadable.path(), {make_header(unreadable.path(), 100, 100, "t", "m")});
        const ScopedPermissions permissions(db_path_of(unreadable.path()),
                                            std::filesystem::perms::none);
        const WorkspaceHistory history = classify(record_for(unreadable.path(), "unreadable"));
        ASSERT_TRUE(history.note.has_value());
        EXPECT_EQ(*history.note, "unreadable");
    }

    // chmod 0555 directory with no sidecars → "read-only location"; non-root.
    if (::geteuid() != 0) {
        TempDir read_only("ymh_catalog_u20_readonly");
        create_valid_db(read_only.path(), {make_header(read_only.path(), 100, 100, "t", "m")});
        remove_sidecars(read_only.path());
        const ScopedPermissions permissions(
            read_only.path() / ".ymh",
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
                std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
        const WorkspaceHistory history = classify(record_for(read_only.path(), "read-only"));
        ASSERT_TRUE(history.note.has_value());
        EXPECT_EQ(*history.note, "read-only location");
    }
}

TEST(SessionCatalogReaderTest, SW_U5_DeliversSnapshotAndBumpsGeneration) {
    SnapshotCollector collector;
    SessionCatalogReader reader(
        single_source([](const WorkspaceRecord& record, bool live) {
            return make_history(record, live);
        }),
        [&collector](SessionCatalogSnapshot snapshot) { collector.push(std::move(snapshot)); },
        std::chrono::milliseconds{1000});
    reader.start();
    reader.refreshNow();

    ASSERT_TRUE(collector.wait_for_count(1, std::chrono::milliseconds{2000}));
    const SessionCatalogSnapshot first = collector.at(0);
    EXPECT_TRUE(first.complete);
    ASSERT_EQ(first.workspaces.size(), 1u);
    EXPECT_EQ(first.workspaces[0].title, "one");
    ASSERT_EQ(first.workspaces[0].sessions.size(), 1u);
    EXPECT_GT(first.capturedAtMs, 0);
    EXPECT_EQ(first.generation, 1u);

    reader.refreshNow();
    ASSERT_TRUE(collector.wait_for_count(2, std::chrono::milliseconds{2000}));
    EXPECT_EQ(collector.at(1).generation, 2u);

    reader.stop();
    reader.stop();
    EXPECT_EQ(collector.count(), 2u);
}

TEST(SessionCatalogReaderTest, SW_U5_RefreshNowCoalesces) {
    std::mutex              gate_mutex;
    std::condition_variable gate;
    bool                    build_entered = false;
    bool                    release_build = false;
    std::atomic<int>        builds{0};

    WorkspaceCatalogSource source = single_source([&](const WorkspaceRecord& record, bool live) {
        builds.fetch_add(1);
        std::unique_lock lock(gate_mutex);
        build_entered = true;
        gate.notify_all();
        gate.wait(lock, [&] { return release_build; });
        return make_history(record, live);
    });

    SnapshotCollector collector;
    SessionCatalogReader reader(
        std::move(source),
        [&collector](SessionCatalogSnapshot snapshot) { collector.push(std::move(snapshot)); },
        std::chrono::milliseconds{1000});
    reader.start();
    reader.refreshNow();
    {
        std::unique_lock lock(gate_mutex);
        ASSERT_TRUE(gate.wait_for(lock, std::chrono::milliseconds{2000},
                                  [&] { return build_entered; }));
    }
    reader.refreshNow();
    reader.refreshNow();
    reader.refreshNow();
    {
        const std::lock_guard lock(gate_mutex);
        release_build = true;
    }
    gate.notify_all();

    ASSERT_TRUE(collector.wait_for_count(2, std::chrono::milliseconds{2000}));
    EXPECT_EQ(builds.load(), 2);
    reader.stop();
}

TEST(SessionCatalogReaderTest, SW_U5_ThrowingReadKeepsThreadAlive) {
    std::atomic<int> calls{0};
    WorkspaceCatalogSource source = single_source([&](const WorkspaceRecord& record, bool live) {
        if (calls.fetch_add(1) == 0) {
            throw std::runtime_error{"transient read failure"};
        }
        return make_history(record, live);
    });

    SnapshotCollector collector;
    SessionCatalogReader reader(
        std::move(source),
        [&collector](SessionCatalogSnapshot snapshot) { collector.push(std::move(snapshot)); },
        std::chrono::milliseconds{20});
    reader.start();
    reader.refreshNow();

    ASSERT_TRUE(collector.wait_for_count(1, std::chrono::milliseconds{2000}));
    EXPECT_GE(calls.load(), 2);
    reader.stop();
    reader.stop();
}

TEST(SessionCatalogIntegration, SW_I4_DegradationWithoutCrashKeepsOtherGroups) {
    TempDir registry_root("ymh_catalog_reg");
    RegistryConfig registry_config;
    registry_config.db_path         = registry_root.path() / "registry.db";
    registry_config.lock_path       = registry_root.path() / "registry.lock";
    registry_config.workspace_roots = {registry_root.path()};
    const std::unique_ptr<WorkspaceRegistry> registry = WorkspaceRegistry::open(registry_config);

    TempDir good("ymh_catalog_good");
    create_valid_db(good.path(), {make_header(good.path(), 100, 5000, "hello", "model-x")});
    registry->registerWorkspace(good.path(), "good");

    TempDir garbage("ymh_catalog_bad");
    write_text_file(db_path_of(garbage.path()), "not-a-sqlite-file");
    registry->registerWorkspace(garbage.path(), "garbage");

    TempDir gone("ymh_catalog_gone");
    registry->registerWorkspace(gone.path(), "gone");
    std::error_code error;
    std::filesystem::remove_all(gone.path(), error);

    SnapshotCollector collector;
    SessionCatalogReader reader(
        *registry,
        [&collector](SessionCatalogSnapshot snapshot) { collector.push(std::move(snapshot)); },
        std::chrono::milliseconds{10});
    reader.start();
    reader.refreshNow();
    ASSERT_TRUE(collector.wait_for_count(1, std::chrono::milliseconds{3000}));
    reader.stop();

    const SessionCatalogSnapshot snapshot = collector.at(0);
    EXPECT_FALSE(snapshot.complete);
    ASSERT_EQ(snapshot.workspaces.size(), 3u);
    for (const WorkspaceHistory& workspace : snapshot.workspaces) {
        if (workspace.title == "good") {
            EXPECT_FALSE(workspace.note.has_value());
            ASSERT_EQ(workspace.sessions.size(), 1u);
            EXPECT_EQ(workspace.sessions[0].title, "hello");
        } else if (workspace.title == "garbage") {
            ASSERT_TRUE(workspace.note.has_value());
            EXPECT_EQ(*workspace.note, "corrupt");
        } else if (workspace.title == "gone") {
            ASSERT_TRUE(workspace.note.has_value());
            EXPECT_EQ(*workspace.note, "workspace missing");
        }
    }
}

TEST(SessionCatalogIntegration, SW_I7_WritableReadSucceedsAndWritesNoRows) {
    TempDir workspace("ymh_catalog_i7");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 4000, "stored", "m")});
    remove_sidecars(workspace.path());

    const std::string before = dump_rows(db_path_of(workspace.path()), "sessions");
    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);
    const std::string after = dump_rows(db_path_of(workspace.path()), "sessions");

    EXPECT_FALSE(history.note.has_value());
    ASSERT_EQ(history.sessions.size(), 1u);
    EXPECT_EQ(history.sessions[0].title, "stored");
    EXPECT_FALSE(before.empty());
    EXPECT_EQ(before, after);
}

TEST(SessionCatalogIntegration, SW_I7_ReadOnlyDirectoryYieldsReadOnlyLocation) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "permission-dependent case; running as root";
    }
    TempDir workspace("ymh_catalog_i7_readonly");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 4000, "stored", "m")});
    remove_sidecars(workspace.path());
    const ScopedPermissions permissions(
        workspace.path() / ".ymh",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec);

    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    ASSERT_TRUE(history.note.has_value());
    EXPECT_EQ(*history.note, "read-only location");
    EXPECT_TRUE(history.sessions.empty());
}

TEST(SessionCatalogIntegration, SW_I7_ReadOnlyDirectoryWithSidecarsStillSucceeds) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "permission-dependent case; running as root";
    }
    TempDir workspace("ymh_catalog_i7_sidecars");
    create_valid_db(workspace.path(), {make_header(workspace.path(), 100, 4000, "stored", "m")});
    remove_sidecars(workspace.path());
    const std::filesystem::path ymh_dir = workspace.path() / ".ymh";

    // A read in the writable directory attaches the WAL sidecars (LOW-D).
    const WorkspaceHistory warm =
        read_workspace_history(record_for(workspace.path(), "ws"), false);
    ASSERT_FALSE(warm.note.has_value());
    ASSERT_TRUE(std::filesystem::exists(ymh_dir / "sessions.db-shm") ||
                std::filesystem::exists(ymh_dir / "sessions.db-wal"));

    const ScopedPermissions permissions(
        ymh_dir,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
    const WorkspaceHistory history =
        read_workspace_history(record_for(workspace.path(), "ws"), false);

    EXPECT_FALSE(history.note.has_value());
    ASSERT_EQ(history.sessions.size(), 1u);
    EXPECT_EQ(history.sessions[0].title, "stored");
}

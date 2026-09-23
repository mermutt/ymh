#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include <sqlite3.h>

#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"

namespace {

using namespace ymh;

std::filesystem::path make_temp_dir() {
    const auto base = std::filesystem::temp_directory_path() /
                      ("ymh_persist_" + std::to_string(::getpid()) + "_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

class TempWorkspace {
public:
    TempWorkspace() : root_(make_temp_dir()) {}

    ~TempWorkspace() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    [[nodiscard]] std::filesystem::path db_path() const { return root_ / ".ymh" / "sessions.db"; }
    [[nodiscard]] std::filesystem::path lock_path() const {
        return root_ / ".ymh" / "sessions.lock";
    }

    [[nodiscard]] PersistenceConfig config(const std::string& boot = "boot-1") const {
        PersistenceConfig cfg;
        cfg.db_path   = db_path();
        cfg.lock_path = lock_path();
        cfg.boot_id   = BootId{boot};
        return cfg;
    }

private:
    std::filesystem::path root_;
};

class RawDb {
public:
    explicit RawDb(const std::filesystem::path& path, bool read_only = false) {
        const int flags = read_only ? (SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX)
                                    : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX);
        if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            throw std::runtime_error("raw open failed");
        }
    }

    ~RawDb() {
        if (db_ != nullptr) {
            sqlite3_close_v2(db_);
        }
    }

    RawDb(const RawDb&) = delete;
    RawDb& operator=(const RawDb&) = delete;

    [[nodiscard]] int exec(const std::string& sql) const {
        return sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    }

    [[nodiscard]] std::int64_t query_int(const std::string& sql) const {
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

    [[nodiscard]] std::string query_text(const std::string& sql) const {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            return {};
        }
        std::string value;
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(statement, 0);
            if (text != nullptr) {
                value = reinterpret_cast<const char*>(text);
            }
        }
        sqlite3_finalize(statement);
        return value;
    }

private:
    sqlite3* db_ = nullptr;
};

SessionHeader make_header(const std::filesystem::path& cwd, SessionKind kind = SessionKind::Root) {
    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = std::filesystem::canonical(cwd);
    header.createdAt     = 1000;
    header.updatedAt     = 1000;
    header.model         = "test-model";
    header.serverProfile = "interactive";
    header.kind          = kind;
    return header;
}

Event make_event(const SessionId& session, std::chrono::system_clock::time_point timestamp) {
    TypedEvent<payload::SessionStarted> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = timestamp;
    typed.payload    = payload::SessionStarted{"test-model", "interactive", "t", ""};
    return encode(typed);
}

Event make_event_with_id(const SessionId& session, const EventId& id) {
    TypedEvent<payload::SessionStarted> typed;
    typed.id         = id;
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::now();
    typed.payload    = payload::SessionStarted{"test-model", "interactive", "t", ""};
    return encode(typed);
}

template <class P>
Event payload_event(const SessionId& session, const P& payload) {
    TypedEvent<P> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::now();
    typed.payload    = payload;
    return encode(typed);
}

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

Event make_rename(const SessionId& session, const std::string& title,
                  payload::RenameOrigin origin = payload::RenameOrigin::User) {
    TypedEvent<payload::SessionRenamed> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::now();
    typed.payload    = payload::SessionRenamed{title, origin};
    return encode(typed);
}

// 23 §3.4/§5.2: the own-log predicate's only positive witness.
Event make_user_message(const SessionId& session, std::string text) {
    TypedEvent<payload::UserMessage> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::now();
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    typed.payload = payload::UserMessage{MessageId{"m-" + session.value}, {block}};
    return encode(typed);
}

// 23 §5.4: the terminal event `eraseWithEvent` appends before erasing.
Event make_session_ended(const SessionId& session) {
    TypedEvent<payload::SessionEnded> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::now();
    typed.payload    = payload::SessionEnded{payload::SessionEndReason::Deleted};
    return encode(typed);
}

// 23 §5.4 gate: a persistence assertion counts rows AND leases AND events AND
// snapshots, never rows alone.
struct RowCounts {
    std::int64_t sessions  = -1;
    std::int64_t leases    = -1;
    std::int64_t events    = -1;
    std::int64_t snapshots = -1;
};

RowCounts count_rows(const std::filesystem::path& db_path, const SessionId& id) {
    RawDb raw(db_path, true);
    return RowCounts{
        raw.query_int("SELECT COUNT(*) FROM sessions WHERE id='" + id.value + "'"),
        raw.query_int("SELECT COUNT(*) FROM session_leases WHERE session_id='" + id.value + "'"),
        raw.query_int("SELECT COUNT(*) FROM events WHERE session_id='" + id.value + "'"),
        raw.query_int("SELECT COUNT(*) FROM session_snapshots WHERE session_id='" + id.value + "'"),
    };
}

TEST(Persistence, OpenAppliesSchemaAndPragmas) {
    TempWorkspace workspace;
    auto          store = SessionPersistence::open(workspace.config());
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->schemaVersion(), kSchemaVersion);

    RawDb raw(workspace.db_path(), true);
    EXPECT_EQ(raw.query_int("PRAGMA application_id"), kApplicationId);
    EXPECT_EQ(raw.query_int("PRAGMA user_version"), kSchemaVersion);
    EXPECT_EQ(raw.query_text("PRAGMA journal_mode"), "wal");
}

TEST(Persistence, ForeignKeysEnforced) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));

    RawDb raw(workspace.db_path());
    ASSERT_EQ(raw.exec("PRAGMA foreign_keys = ON"), SQLITE_OK);
    EXPECT_NE(raw.exec("DELETE FROM sessions WHERE id = '" + header.id.value + "'"), SQLITE_OK);
    EXPECT_NE(raw.exec("INSERT INTO events(session_id, event_id, timestamp, type, payload) VALUES "
                       "('missing','e1',0,'session/start','{}')"),
              SQLITE_OK);
}

TEST(Persistence, SchemaCheckConstraints) {
    TempWorkspace workspace;
    auto          store = SessionPersistence::open(workspace.config());
    RawDb         raw(workspace.db_path());
    ASSERT_EQ(raw.exec("PRAGMA foreign_keys = ON"), SQLITE_OK);

    ASSERT_EQ(raw.exec("INSERT INTO sessions(id,cwd,created_at,updated_at,kind,parent_session,"
                       "seed_length) VALUES ('p','/tmp',0,0,'root',NULL,NULL)"),
              SQLITE_OK);
    EXPECT_NE(raw.exec("INSERT INTO sessions(id,cwd,created_at,updated_at,kind,parent_session,"
                       "seed_length) VALUES ('x','/tmp',0,0,'fork',NULL,NULL)"),
              SQLITE_OK);
    EXPECT_NE(raw.exec("INSERT INTO sessions(id,cwd,created_at,updated_at,kind,parent_session,"
                       "seed_length) VALUES ('y','/tmp',0,0,'root','p',NULL)"),
              SQLITE_OK);
    EXPECT_NE(raw.exec("INSERT INTO sessions(id,cwd,created_at,updated_at,kind,parent_session,"
                       "seed_length) VALUES ('z','/tmp',0,0,'subagent','p',5)"),
              SQLITE_OK);
    EXPECT_EQ(raw.exec("INSERT INTO sessions(id,cwd,created_at,updated_at,kind,parent_session,"
                       "seed_length) VALUES ('ok','/tmp',0,0,'subagent','p',0)"),
              SQLITE_OK);
}

TEST(Persistence, CreateLoadListRoundTrip) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    SessionHeader header = make_header(workspace.root());
    header.title         = "round trip";
    header.metadata      = R"({"k":"v"})";
    store->create(header);

    const auto loaded = store->load(header.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, header);
    EXPECT_TRUE(store->isLeaseHolder(header.id));

    const auto headers = store->list();
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers.front().id.value, header.id.value);
}

// 54-U8 (54-D8): the durable `sessions` metadata envelope recovers `model_name`
// without depending on an event.
TEST(Persistence, ModelNameRoundTripsThroughMetadataColumn) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    SessionHeader header = make_header(workspace.root());
    header.model_name    = std::string{"fast"};
    store->create(header);

    const auto loaded = store->load(header.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->model_name, std::optional<std::string>{"fast"});
    EXPECT_EQ(*loaded, header);
}

// A pre-54 row without a name loads with nullopt (durable resolution falls back
// to the wire id).
TEST(Persistence, MissingModelNameLoadsNullopt) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    SessionHeader header = make_header(workspace.root());
    store->create(header);

    const auto loaded = store->load(header.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_FALSE(loaded->model_name.has_value());
}

TEST(Persistence, SequenceMonotonicWithGaps) {
    TempWorkspace workspace;
    auto          store = SessionPersistence::open(workspace.config());
    const SessionId a   = store->create(make_header(workspace.root())).id;
    const SessionId b   = store->create(make_header(workspace.root())).id;

    const Sequence a1 = store->append(a, make_event(a, std::chrono::system_clock::now()));
    const Sequence b1 = store->append(b, make_event(b, std::chrono::system_clock::now()));
    const Sequence a2 = store->append(a, make_event(a, std::chrono::system_clock::now()));

    EXPECT_LT(a1, b1);
    EXPECT_LT(b1, a2);
    EXPECT_GT(a2, a1 + 1);
    const EventRange events = store->read(a);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_LT(events[0].seq, events[1].seq);
}

TEST(Persistence, AppendBatchAtomicAndOrdered) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionId session = store->create(make_header(workspace.root())).id;

    std::vector<Event> batch{make_event(session, std::chrono::system_clock::now()),
                             make_event(session, std::chrono::system_clock::now())};
    const std::vector<Sequence> sequences = store->appendBatch(session, batch);
    ASSERT_EQ(sequences.size(), 2u);
    EXPECT_LT(sequences[0], sequences[1]);

    const std::size_t before = store->read(session).size();

    const EventId duplicate = make_event_id();
    std::vector<Event> invalid{make_event_with_id(session, duplicate),
                               make_event_with_id(session, duplicate)};
    EXPECT_THROW(store->appendBatch(session, invalid), CorruptionError);
    EXPECT_EQ(store->read(session).size(), before);
}

TEST(Persistence, UpdatedAtTracksLastEvent) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    const auto timestamp = std::chrono::system_clock::time_point{std::chrono::milliseconds{123456}};
    store->append(header.id, make_event(header.id, timestamp));

    const auto loaded = store->load(header.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->updatedAt, 123456);
}

TEST(Persistence, NonHolderAppendThrowsLeaseLost) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    EXPECT_TRUE(store->releaseLease(header.id));
    EXPECT_FALSE(store->isLeaseHolder(header.id));
    EXPECT_THROW(store->append(header.id, make_event(header.id, std::chrono::system_clock::now())),
                 LeaseLost);
    EXPECT_TRUE(store->read(header.id).empty());
}

TEST(Persistence, LeaseAcquireRenewStealRelease) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    const SessionId session    = header.id;

    EXPECT_EQ(store->leaseState(session), LeaseState::HeldByMe);
    EXPECT_TRUE(store->acquireLease(session));
    store->renewLeases();
    EXPECT_TRUE(store->isLeaseHolder(session));

    ASSERT_TRUE(store->releaseLease(session));
    EXPECT_EQ(store->leaseState(session), LeaseState::Absent);

    RawDb raw(workspace.db_path());
    ASSERT_EQ(raw.exec("INSERT INTO session_leases(session_id, holder_pid, holder_boot_id, "
                       "acquired_at, expires_at) VALUES ('" +
                       session.value + "', 999999, 'dead', 0, 0)"),
              SQLITE_OK);
    EXPECT_TRUE(store->acquireLease(session));
    EXPECT_EQ(store->leaseState(session), LeaseState::HeldByMe);

    ASSERT_EQ(raw.exec("UPDATE session_leases SET holder_pid = " + std::to_string(::getpid()) +
                       ", holder_boot_id = 'other', expires_at = 99999999999999 WHERE session_id "
                       "= '" +
                       session.value + "'"),
              SQLITE_OK);
    EXPECT_FALSE(store->acquireLease(session));
    EXPECT_EQ(store->leaseState(session), LeaseState::HeldByOther);
}

TEST(Persistence, SecondWriterRejectedWhileFlockHeld) {
    TempWorkspace workspace;
    auto          store = SessionPersistence::open(workspace.config("boot-a"));
    ASSERT_NE(store, nullptr);
    EXPECT_THROW(SessionPersistence::open(workspace.config("boot-b")), StoreOpenError);
}

TEST(Persistence, HeldFlockReportsLockedCode) {
    TempWorkspace workspace;
    auto          store = SessionPersistence::open(workspace.config("boot-a"));
    ASSERT_NE(store, nullptr);
    try {
        SessionPersistence::open(workspace.config("boot-b"));
        FAIL() << "expected StoreOpenError";
    } catch (const StoreOpenError& error) {
        EXPECT_EQ(error.code(), StoreOpenErrorCode::Locked);
    }
}

TEST(Persistence, MissingWorkspaceRootReportsUnavailableCode) {
    TempWorkspace workspace;
    PersistenceConfig config = workspace.config();
    config.db_path   = workspace.root() / "missing" / ".ymh" / "sessions.db";
    config.lock_path = workspace.root() / "missing" / ".ymh" / "sessions.lock";
    try {
        SessionPersistence::open(config);
        FAIL() << "expected StoreOpenError";
    } catch (const StoreOpenError& error) {
        EXPECT_EQ(error.code(), StoreOpenErrorCode::Unavailable);
    }
}

TEST(Persistence, ReadOnlyOpenCoexistsWithWriter) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));

    auto reader = SessionPersistence::openReadOnly(workspace.config("boot-reader"));
    ASSERT_NE(reader, nullptr);
    EXPECT_FALSE(reader->isLeaseHolder(header.id));
    EXPECT_EQ(reader->read(header.id).size(), 1u);
    EXPECT_THROW(reader->append(header.id, make_event(header.id, std::chrono::system_clock::now())),
                 StoreOpenError);
    EXPECT_THROW(reader->erase(header.id), StoreOpenError);
}

TEST(Persistence, ReadOnlyMissingDatabaseRejected) {
    TempWorkspace workspace;
    EXPECT_THROW(SessionPersistence::openReadOnly(workspace.config()), StoreOpenError);
}

TEST(Persistence, ForkResolvesSharedPrefix) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader parent = store->create(make_header(workspace.root()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));

    SessionHeader child = make_header(workspace.root(), SessionKind::Fork);
    child.parentSession = parent.id;
    child.seedLength    = 2;
    store->create(child);
    store->append(child.id, make_event(child.id, std::chrono::system_clock::now()));

    const EventRange parentView = store->read(parent.id);
    const EventRange childView  = store->read(child.id);
    ASSERT_EQ(parentView.size(), 3u);
    ASSERT_EQ(childView.size(), 3u);
    EXPECT_EQ(childView[0].seq, parentView[0].seq);
    EXPECT_EQ(childView[1].seq, parentView[1].seq);
    EXPECT_GT(childView[2].seq, parentView[1].seq);
}

TEST(Persistence, HeadSequenceIsHighestCommitted) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    EXPECT_EQ(store->headSequence(header.id), 0);

    const Sequence first  = store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    const Sequence second = store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    EXPECT_GT(second, first);
    EXPECT_EQ(store->headSequence(header.id), second);
}

TEST(Persistence, HeadSequenceUnknownSessionThrows) {
    TempWorkspace workspace;
    auto          store = SessionPersistence::open(workspace.config());
    EXPECT_THROW(static_cast<void>(store->headSequence(SessionId{"00000000-0000-4000-8000-000000000000"})),
                 UnknownSession);
}

TEST(Persistence, ReadAfterIsBoundedAndAscending) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    const Sequence first  = store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    const Sequence second = store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    const Sequence third  = store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));

    EXPECT_TRUE(store->readAfter(header.id, 0, 0).empty());

    const EventRange first_two = store->readAfter(header.id, 0, 2);
    ASSERT_EQ(first_two.size(), 2u);
    EXPECT_EQ(first_two[0].seq, first);
    EXPECT_EQ(first_two[1].seq, second);

    const EventRange rest = store->readAfter(header.id, second, kUnbounded);
    ASSERT_EQ(rest.size(), 1u);
    EXPECT_EQ(rest[0].seq, third);

    const EventRange all = store->readAfter(header.id, 0, kUnbounded);
    EXPECT_EQ(all.size(), store->read(header.id).size());
}

TEST(Persistence, ReadAfterForkPrefixHonoursLimit) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader parent = store->create(make_header(workspace.root()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));

    SessionHeader child = make_header(workspace.root(), SessionKind::Fork);
    child.parentSession = parent.id;
    child.seedLength    = 2;
    store->create(child);
    store->append(child.id, make_event(child.id, std::chrono::system_clock::now()));

    const EventRange childView = store->read(child.id);
    ASSERT_EQ(childView.size(), 3u);
    EXPECT_EQ(store->headSequence(child.id), childView.back().seq);

    const EventRange bounded = store->readAfter(child.id, 0, 2);
    ASSERT_EQ(bounded.size(), 2u);
    EXPECT_EQ(bounded[0].seq, childView[0].seq);
    EXPECT_EQ(bounded[1].seq, childView[1].seq);

    const EventRange tail = store->readAfter(child.id, bounded[1].seq, kUnbounded);
    ASSERT_EQ(tail.size(), 1u);
    EXPECT_EQ(tail[0].seq, childView[2].seq);
}

TEST(Persistence, ForkInvalidBoundaryRejectedAtRead) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader parent = store->create(make_header(workspace.root()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));

    SessionHeader child = make_header(workspace.root(), SessionKind::Fork);
    child.parentSession = parent.id;
    child.seedLength    = 5;
    store->create(child);
    EXPECT_THROW(store->read(child.id), CorruptionError);
}

TEST(Persistence, EraseRefusesDependentsThenRemoves) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader parent = store->create(make_header(workspace.root()));
    SessionHeader child        = make_header(workspace.root(), SessionKind::Fork);
    child.parentSession        = parent.id;
    child.seedLength           = 0;
    store->create(child);

    EXPECT_THROW(store->erase(parent.id), DependentSessionError);
    EXPECT_NO_THROW(store->erase(child.id));
    EXPECT_NO_THROW(store->erase(parent.id));
    EXPECT_FALSE(store->load(parent.id).has_value());
    EXPECT_FALSE(store->load(child.id).has_value());
}

// 23 §3.4: unprompted is own-log `user/message` absence; the title is never
// consulted (SL8).
TEST(SessionStoreSeam, SL5_SL8_IsUnpromptedOwnLogAndTitleIndependent) {
    TempWorkspace workspace;
    auto          store        = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));

    EXPECT_TRUE(store->isUnprompted(header.id));
    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    EXPECT_TRUE(store->isUnprompted(header.id));
    store->append(header.id, make_rename(header.id, "a real title"));
    EXPECT_TRUE(store->isUnprompted(header.id));
    store->append(header.id, make_user_message(header.id, "hi"));
    EXPECT_FALSE(store->isUnprompted(header.id));
}

// 23 §3.4: the query is over OWN events, so a fork's parent prefix cannot mark
// the fork as prompted.
TEST(SessionStoreSeam, SL5_IsUnpromptedIgnoresForkParentPrefix) {
    TempWorkspace workspace;
    auto          store        = SessionPersistence::open(workspace.config());
    const SessionHeader parent = store->create(make_header(workspace.root()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));
    store->append(parent.id, make_user_message(parent.id, "parent prompt"));
    ASSERT_FALSE(store->isUnprompted(parent.id));

    SessionHeader child = make_header(workspace.root(), SessionKind::Fork);
    child.parentSession = parent.id;
    child.seedLength    = 2;
    store->create(child);

    EXPECT_TRUE(store->isUnprompted(child.id));
    store->append(child.id, make_event(child.id, std::chrono::system_clock::now()));
    EXPECT_TRUE(store->isUnprompted(child.id));
    store->append(child.id, make_user_message(child.id, "child prompt"));
    EXPECT_FALSE(store->isUnprompted(child.id));
}

// 23 §5.3: hasDependents is true for the parent only, and clears once the
// dependent is gone.
TEST(SessionStoreSeam, SL_U4_HasDependentsParentOnly) {
    TempWorkspace workspace;
    auto          store        = SessionPersistence::open(workspace.config());
    const SessionHeader parent = store->create(make_header(workspace.root()));
    const SessionHeader other  = store->create(make_header(workspace.root()));
    EXPECT_FALSE(store->hasDependents(parent.id));
    EXPECT_FALSE(store->hasDependents(other.id));

    SessionHeader child = make_header(workspace.root(), SessionKind::Fork);
    child.parentSession = parent.id;
    child.seedLength    = 0;
    store->create(child);

    EXPECT_TRUE(store->hasDependents(parent.id));
    EXPECT_FALSE(store->hasDependents(child.id));
    EXPECT_FALSE(store->hasDependents(other.id));

    store->erase(child.id);
    EXPECT_FALSE(store->hasDependents(parent.id));
}

// 23 §5.4: eraseWithEvent removes the row, the lease, the events, and the
// snapshot in one transaction.
TEST(SessionStoreSeam, SL11_EraseWithEventRemovesAllRowsLeasesAndEvents) {
    TempWorkspace workspace;
    auto          store        = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    store->checkpoint(header.id);

    const RowCounts before = count_rows(workspace.db_path(), header.id);
    ASSERT_EQ(before.sessions, 1);
    ASSERT_EQ(before.leases, 1);
    ASSERT_EQ(before.events, 1);
    ASSERT_EQ(before.snapshots, 1);

    store->eraseWithEvent(header.id, make_session_ended(header.id));
    EXPECT_FALSE(store->load(header.id).has_value());

    const RowCounts after = count_rows(workspace.db_path(), header.id);
    EXPECT_EQ(after.sessions, 0);
    EXPECT_EQ(after.leases, 0);
    EXPECT_EQ(after.events, 0);
    EXPECT_EQ(after.snapshots, 0);
}

// AL-U10/AL16/AL17: `eraseWithEvent` returns the store-assigned terminal
// `Sequence`, and `SessionManager::deleteSession` publishes `{seq,
// SessionEnded}` on the committed channel even though the row was erased.
TEST(SessionStoreSeam, AL_U10_EraseWithEventReturnsSequenceAndManagerPublishesCommitted) {
    TempWorkspace workspace;
    auto          store        = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    const Sequence prior =
        store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));

    const Sequence terminal = store->eraseWithEvent(header.id, make_session_ended(header.id));
    EXPECT_GT(terminal, prior);
    EXPECT_FALSE(store->load(header.id).has_value());

    const SessionHeader second = store->create(make_header(workspace.root()));
    EventBus             bus;
    SessionManager       manager(*store, bus);
    std::vector<EventRecord> committed;
    auto subscription = bus.subscribeCommitted(
        [&committed](const EventRecord& record) { committed.push_back(record); });

    manager.deleteSession(second.id, false);

    ASSERT_EQ(committed.size(), 1u);
    EXPECT_EQ(committed[0].event.type, EventType::SessionEnded);
    EXPECT_EQ(committed[0].event.session_id.value, second.id.value);
    EXPECT_EQ(committed[0].event.payload.get<payload::SessionEnded>().reason,
              payload::SessionEndReason::Deleted);
    EXPECT_GT(committed[0].seq, 0);
}
TEST(SessionStoreSeam, SL11_EraseWithEventIsLeaseExempt) {
    TempWorkspace workspace;
    auto          store        = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    ASSERT_TRUE(store->releaseLease(header.id));
    ASSERT_FALSE(store->isLeaseHolder(header.id));
    EXPECT_THROW(store->append(header.id, make_event(header.id, std::chrono::system_clock::now())),
                 LeaseLost);

    EXPECT_NO_THROW(store->eraseWithEvent(header.id, make_session_ended(header.id)));
    EXPECT_FALSE(store->load(header.id).has_value());
    const RowCounts after = count_rows(workspace.db_path(), header.id);
    EXPECT_EQ(after.sessions, 0);
    EXPECT_EQ(after.leases, 0);
    EXPECT_EQ(after.events, 0);
    EXPECT_EQ(after.snapshots, 0);
}

// 23 §5.4: a dependent parent is refused and the whole transaction (including
// the appended terminal event) rolls back -- parent rows/leases/events intact.
TEST(SessionStoreSeam, SL_U4_SL11_EraseWithEventRefusesDependentsAndRollsBack) {
    TempWorkspace workspace;
    auto          store        = SessionPersistence::open(workspace.config());
    const SessionHeader parent = store->create(make_header(workspace.root()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));
    store->append(parent.id, make_rename(parent.id, "keep me"));
    store->checkpoint(parent.id);

    SessionHeader child = make_header(workspace.root(), SessionKind::Fork);
    child.parentSession = parent.id;
    child.seedLength    = 0;
    store->create(child);

    const RowCounts before = count_rows(workspace.db_path(), parent.id);
    ASSERT_EQ(before.sessions, 1);
    ASSERT_EQ(before.leases, 1);
    ASSERT_EQ(before.events, 2);
    ASSERT_EQ(before.snapshots, 1);

    EXPECT_THROW(store->eraseWithEvent(parent.id, make_session_ended(parent.id)),
                 DependentSessionError);

    const RowCounts after = count_rows(workspace.db_path(), parent.id);
    EXPECT_EQ(after.sessions, 1);
    EXPECT_EQ(after.leases, 1);
    EXPECT_EQ(after.events, 2);
    EXPECT_EQ(after.snapshots, 1);
    EXPECT_TRUE(store->load(parent.id).has_value());
    EXPECT_TRUE(store->hasDependents(parent.id));
}

TEST(Persistence, SnapshotCheckpointLifecycle) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));

    store->checkpoint(header.id);
    EXPECT_TRUE(store->snapshotIsCurrent(header.id));
    const auto snapshot = store->loadSnapshot(header.id);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->eventCount, 1u);
    EXPECT_EQ(snapshot->messages.size(), 0u);

    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    EXPECT_FALSE(store->snapshotIsCurrent(header.id));
    store->discardSnapshot(header.id);
    EXPECT_FALSE(store->loadSnapshot(header.id).has_value());
}

TEST(Persistence, ProvenanceRoundTripsThroughDb) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));

    payload::UserMessage user;
    user.id      = "u1";
    user.content = {text_block("hi")};

    payload::ContextInjected injected;
    injected.id      = "c1";
    injected.role    = Role::User;
    injected.text    = "instructions";
    injected.context = ContextFormed{ContextForm::Instructions};
    injected.source  = plugin_message_source("agent-instructions", injected.context);

    payload::AssistantMessage assistant;
    assistant.id      = "a1";
    assistant.content = {text_block("answer")};
    assistant.source  = model_message_source("deepseek", "deepseek-flash");

    payload::ToolResult tool;
    tool.id          = "t1";
    tool.name        = "read_file";
    tool.output      = "body";
    tool.source.call = tool.id;

    store->append(header.id, payload_event(header.id, user));
    store->append(header.id, payload_event(header.id, injected));
    store->append(header.id, payload_event(header.id, assistant));
    store->append(header.id, payload_event(header.id, tool));

    const EventRange events = store->read(header.id);
    ASSERT_EQ(events.size(), 4u);

    const auto& stored_user = events[0].event.payload.get<payload::UserMessage>();
    EXPECT_EQ(stored_user.source.kind, MessageSource::Kind::User);

    const auto& stored_injected = events[1].event.payload.get<payload::ContextInjected>();
    EXPECT_EQ(stored_injected.source.plugin, "agent-instructions");
    EXPECT_EQ(stored_injected.context.form, ContextForm::Instructions);

    const auto& stored_assistant = events[2].event.payload.get<payload::AssistantMessage>();
    EXPECT_EQ(stored_assistant.source.provider, "deepseek");
    EXPECT_EQ(stored_assistant.source.model, "deepseek-flash");

    const auto& stored_tool = events[3].event.payload.get<payload::ToolResult>();
    ASSERT_TRUE(stored_tool.source.call.has_value());
    EXPECT_EQ(*stored_tool.source.call, "t1");

    const std::vector<Message> messages = deriveMessages(header, events);
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_TRUE(messages[1].source == stored_injected.source);
    EXPECT_TRUE(messages[1].context == stored_injected.context);
    EXPECT_TRUE(messages[3].source == stored_tool.source);
}

TEST(Persistence, PreAmendmentMessagesCompareUnequalToFreshProjection) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));

    payload::AssistantMessage assistant;
    assistant.id      = "a1";
    assistant.content = {text_block("answer")};
    assistant.source  = model_message_source("deepseek", "deepseek-flash");
    store->append(header.id, payload_event(header.id, assistant));

    nlohmann::json pre_amendment = nlohmann::json::array();
    pre_amendment.push_back(
        {{"role", "assistant"}, {"content", nlohmann::json::array()}, {"tool_call_id", ""}});
    const std::vector<Message> decoded = pre_amendment.get<std::vector<Message>>();
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_FALSE(decoded[0].source.has_value());

    const std::vector<Message> fresh = deriveMessages(header, store->read(header.id));
    ASSERT_EQ(fresh.size(), 1u);
    EXPECT_FALSE(decoded[0] == fresh[0]);
}

TEST(Persistence, DuplicateEventIdRejected) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    const EventId id           = make_event_id();
    store->append(header.id, make_event_with_id(header.id, id));
    EXPECT_THROW(store->append(header.id, make_event_with_id(header.id, id)), CorruptionError);
    EXPECT_EQ(store->read(header.id).size(), 1u);
}

TEST(Persistence, PayloadCapRejected) {
    TempWorkspace workspace;
    PersistenceConfig config = workspace.config();
    config.max_payload_bytes = 8;
    auto          store  = SessionPersistence::open(config);
    const SessionHeader header = store->create(make_header(workspace.root()));
    EXPECT_THROW(store->append(header.id, make_event(header.id, std::chrono::system_clock::now())),
                 PayloadTooLarge);
    EXPECT_TRUE(store->read(header.id).empty());
}

TEST(Persistence, NewerSchemaRefused) {
    TempWorkspace workspace;
    {
        auto store = SessionPersistence::open(workspace.config());
        store->close();
    }
    RawDb raw(workspace.db_path());
    ASSERT_EQ(raw.exec("PRAGMA user_version = 99"), SQLITE_OK);
    EXPECT_THROW(SessionPersistence::open(workspace.config()), SchemaVersionError);
}

TEST(Persistence, ForeignApplicationIdRefused) {
    TempWorkspace workspace;
    {
        auto store = SessionPersistence::open(workspace.config());
        store->close();
    }
    RawDb raw(workspace.db_path());
    ASSERT_EQ(raw.exec("PRAGMA application_id = 12345"), SQLITE_OK);
    EXPECT_THROW(SessionPersistence::open(workspace.config()), SchemaVersionError);
}

TEST(BootNonce, MintIsUuidV4AndAdaptersRoundTrip) {
    const BootId boot = mint_boot_id();
    ASSERT_EQ(boot.value.size(), 36u);
    EXPECT_EQ(boot.value[14], '4');
    EXPECT_EQ(boot.value[8], '-');
    EXPECT_NE(mint_boot_id().value, boot.value);

    const HostBootId host = to_host_boot_id(boot);
    EXPECT_EQ(host.value, boot.value);
    EXPECT_EQ(to_boot_id(host).value, boot.value);
    EXPECT_EQ(to_protocol_boot_id(host).value, boot.value);
}

TEST(PersistenceRename, AppendMaterializesTitleAndAdvancesUpdatedAt) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    const auto timestamp = std::chrono::system_clock::time_point{std::chrono::milliseconds{777000}};
    Event rename = make_rename(header.id, "renamed");
    rename.timestamp = timestamp;
    store->append(header.id, rename);

    const auto loaded = store->load(header.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->title, "renamed");
    EXPECT_EQ(loaded->updatedAt, 777000);
    EXPECT_EQ(store->list()[0].title, "renamed");

    auto read_only = SessionPersistence::openReadOnly(workspace.config());
    const auto reopened = read_only->load(header.id);
    ASSERT_TRUE(reopened.has_value());
    EXPECT_EQ(reopened->title, "renamed");
}

TEST(PersistenceRename, ReloadReconcilesHeaderFromOwnLogButNotColumn) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));
    store->append(header.id, make_rename(header.id, "from-own-log"));

    RawDb raw(workspace.db_path());
    ASSERT_EQ(raw.exec("UPDATE sessions SET title = 'stale' WHERE id = '" + header.id.value + "'"),
              SQLITE_OK);

    EventBus bus;
    SessionHeader loaded = *store->load(header.id);
    ASSERT_EQ(loaded.title, "stale");
    Session session = Session::resume(loaded, *store, bus);
    EXPECT_EQ(session.header().title, "from-own-log");

    const auto column = store->load(header.id);
    ASSERT_TRUE(column.has_value());
    EXPECT_EQ(column->title, "stale");
}

TEST(PersistenceRename, ForkDoesNotInheritParentTitle) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader parent = store->create(make_header(workspace.root()));
    store->append(parent.id, make_event(parent.id, std::chrono::system_clock::now()));
    store->append(parent.id, make_rename(parent.id, "parent renamed"));

    SessionHeader child = make_header(workspace.root(), SessionKind::Fork);
    child.parentSession = parent.id;
    child.seedLength    = 2;
    store->create(child);

    EventBus bus;
    Session fork = Session::resume(*store->load(child.id), *store, bus);
    EXPECT_EQ(fork.header().title, "");
    EXPECT_TRUE(fork.ownEvents().empty());

    const auto child_header = store->load(child.id);
    ASSERT_TRUE(child_header.has_value());
    EXPECT_EQ(child_header->title, "");
    for (const SessionHeader& listed : store->list()) {
        if (listed.id == child.id) {
            EXPECT_EQ(listed.title, "");
        }
    }

    store->append(child.id, make_rename(child.id, "child renamed"));
    EXPECT_EQ(store->load(child.id)->title, "child renamed");
    EXPECT_EQ(store->load(parent.id)->title, "parent renamed");
}

TEST(PersistenceRename, ReplayReproducesFinalTitleAndMessages) {
    TempWorkspace workspace;
    auto          store  = SessionPersistence::open(workspace.config());
    const SessionHeader header = store->create(make_header(workspace.root()));
    store->append(header.id, make_event(header.id, std::chrono::system_clock::now()));

    TypedEvent<payload::UserMessage> message;
    message.id         = make_event_id();
    message.session_id = header.id;
    message.timestamp  = std::chrono::system_clock::now();
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "hello";
    message.payload = payload::UserMessage{MessageId{"m1"}, {block}};
    store->append(header.id, encode(message));

    store->append(header.id, make_rename(header.id, "first"));
    store->append(header.id, make_rename(header.id, "second", payload::RenameOrigin::Auto));

    EventBus bus;
    Session replayed = Session::replay(*store->load(header.id), *store, bus);
    EXPECT_EQ(replayed.header().title, "second");
    const std::vector<Message> messages = replayed.deriveMessages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].role, Role::User);
    ASSERT_EQ(messages[0].content.size(), 1u);
    EXPECT_EQ(messages[0].content[0].text, "hello");

    std::size_t renames = 0;
    for (const EventRecord& record : replayed.ownEvents()) {
        if (record.event.type == EventType::SessionRenamed) {
            ++renames;
        }
    }
    EXPECT_EQ(renames, 2u);
}

} // namespace

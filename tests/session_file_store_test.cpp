#include <gtest/gtest.h>

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "test_support.hpp"

#include "ymh/core/event.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/session_persistence.hpp"

namespace {

using ymh::Event;
using ymh::EventType;
using ymh::PayloadTooLarge;
using ymh::SessionHeader;
using ymh::SessionId;
using ymh::SessionPersistence;
using ymh_test::TempWorkspace;
using ymh_test::event_for;
using ymh_test::fork_header;
using ymh_test::fresh_uuid;
using ymh_test::root_header;
using ymh_test::started_payload;
using ymh_test::user_payload;

ymh::SessionId sid(const std::string& value) { return ymh::SessionId{value}; }

TEST(SessionFileStore, CreateAppendLoadListAndRead) {
    TempWorkspace workspace;
    const auto    config = workspace.persistence();

    const std::string id = fresh_uuid();
    {
        auto store = SessionPersistence::open(config);
        store->create(root_header(workspace.root, id));

        EXPECT_EQ(store->append(sid(id), event_for(id, EventType::SessionStarted, started_payload())), 1);
        EXPECT_EQ(store->append(sid(id), event_for(id, EventType::UserMessage, user_payload("hi"))), 2);

        EXPECT_EQ(store->headSequence(sid(id)), 2);
        const ymh::EventRange events = store->read(sid(id));
        ASSERT_EQ(events.size(), 2U);
        EXPECT_EQ(events[0].seq, 1);
        EXPECT_EQ(events[1].seq, 2);
        EXPECT_FALSE(store->isUnprompted(sid(id)));

        const std::vector<SessionHeader> headers = store->list();
        ASSERT_EQ(headers.size(), 1U);
        EXPECT_EQ(headers.front().id.value, id);
        EXPECT_GT(headers.front().updatedAt, 0);

        store->append(sid(id), event_for(id, EventType::SessionRenamed,
                                    nlohmann::json{{"title", "renamed"}, {"origin", "user"}}));
        const std::optional<SessionHeader> renamed = store->load(sid(id));
        ASSERT_TRUE(renamed.has_value());
        EXPECT_EQ(renamed->title, "renamed");

        store->append(sid(id), event_for(id, EventType::TurnStarted,
                                    nlohmann::json{{"turn", 1}, {"origin", "user"}}));
        EXPECT_EQ(store->readAfter(sid(id), 2, 10).size(), 2U);
        EXPECT_EQ(store->headSequence(sid(id)), 4);
    }

    EXPECT_TRUE(std::filesystem::is_regular_file(workspace.root / ".ymh" / "sessions" /
                                                 id / "header.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(workspace.root / ".ymh" / "sessions" /
                                                 id / "events.jsonl"));
    EXPECT_FALSE(std::filesystem::exists(workspace.root / ".ymh" / "sessions.db"));

    auto read_only = SessionPersistence::openReadOnly(config);
    ASSERT_EQ(read_only->list().size(), 1U);
    EXPECT_EQ(read_only->read(sid(id)).size(), 4U);
    EXPECT_FALSE(read_only->isLeaseHolder(sid(id)));
    EXPECT_EQ(read_only->leaseState(sid(id)), ymh::LeaseState::Absent);
}

TEST(SessionFileStore, ForkAndForkOfForkResolveAscending) {
    TempWorkspace workspace;
    const auto    config = workspace.persistence();

    const std::string parent = fresh_uuid();
    const std::string child  = fresh_uuid();
    const std::string grand  = fresh_uuid();

    auto store = SessionPersistence::open(config);
    store->create(root_header(workspace.root, parent));
    for (int index = 1; index <= 3; ++index) {
        store->append(sid(parent), event_for(parent, EventType::UserMessage,
                                        user_payload("parent " + std::to_string(index))));
    }

    store->create(fork_header(workspace.root, child, parent, 2));
    EXPECT_EQ(store->append(sid(child), event_for(child, EventType::SessionStarted, started_payload())),
              3);
    EXPECT_EQ(store->headSequence(sid(child)), 3);

    const ymh::EventRange child_view = store->read(sid(child));
    ASSERT_EQ(child_view.size(), 3U);
    EXPECT_EQ(child_view[0].seq, 1);
    EXPECT_EQ(child_view[1].seq, 2);
    EXPECT_EQ(child_view[2].seq, 3);

    store->create(fork_header(workspace.root, grand, child, 3));
    EXPECT_EQ(store->append(sid(grand), event_for(grand, EventType::SessionStarted, started_payload())),
              4);
    const ymh::EventRange grand_view = store->read(sid(grand));
    ASSERT_EQ(grand_view.size(), 4U);
    EXPECT_EQ(grand_view.back().seq, 4);

    const std::optional<SessionHeader> child_header = store->load(sid(child));
    ASSERT_TRUE(child_header.has_value());
    ASSERT_TRUE(child_header->parentSession.has_value());
    EXPECT_EQ(child_header->parentSession->value, parent);
    ASSERT_TRUE(child_header->seedLength.has_value());
    EXPECT_EQ(*child_header->seedLength, 2U);

    EXPECT_TRUE(store->hasDependents(sid(parent)));
    EXPECT_TRUE(store->hasDependents(sid(child)));
    EXPECT_THROW(store->erase(sid(parent)), ymh::DependentSessionError);
}

TEST(SessionFileStore, TornTailIsIgnoredAndRepaired) {
    TempWorkspace workspace;
    const auto    config = workspace.persistence();
    const std::string id = fresh_uuid();

    {
        auto store = SessionPersistence::open(config);
        store->create(root_header(workspace.root, id));
        store->append(sid(id), event_for(id, EventType::SessionStarted, started_payload()));
        store->close();
    }

    const std::filesystem::path events =
        workspace.root / ".ymh" / "sessions" / id / "events.jsonl";
    {
        std::ofstream stream{events, std::ios::binary | std::ios::app};
        ASSERT_TRUE(stream.good());
        stream << R"({"seq":999,"partial":)";
    }
    {
        auto read_only = SessionPersistence::openReadOnly(config);
        EXPECT_EQ(read_only->read(sid(id)).size(), 1U);
        EXPECT_EQ(read_only->headSequence(sid(id)), 1);
    }
    {
        auto store = SessionPersistence::open(config);
        ASSERT_TRUE(store->acquireLease(sid(id)));
        EXPECT_EQ(store->append(sid(id), event_for(id, EventType::UserMessage, user_payload("after"))),
                  2);
        EXPECT_EQ(store->read(sid(id)).size(), 2U);
    }

    std::ifstream input{events, std::ios::binary};
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("partial"), std::string::npos);
    EXPECT_EQ(content.back(), '\n');
}

TEST(SessionFileStore, EraseChecksDependentsAndErases) {
    TempWorkspace workspace;
    const auto    config = workspace.persistence();
    const std::string parent = fresh_uuid();
    const std::string child  = fresh_uuid();

    auto store = SessionPersistence::open(config);
    store->create(root_header(workspace.root, parent));
    store->create(fork_header(workspace.root, child, parent, 0));

    EXPECT_THROW(store->erase(sid(parent)), ymh::DependentSessionError);
    EXPECT_NO_THROW(store->erase(sid(child)));
    EXPECT_NO_THROW(store->erase(sid(parent)));
    EXPECT_TRUE(store->list().empty());

    const std::string ended = fresh_uuid();
    store->create(root_header(workspace.root, ended));
    store->append(sid(ended), event_for(ended, EventType::SessionStarted, started_payload()));
    const ymh::Sequence sequence = store->eraseWithEvent(
        sid(ended), event_for(ended, EventType::SessionEnded, nlohmann::json{{"reason", "deleted"}}));
    EXPECT_EQ(sequence, 2);
    EXPECT_FALSE(store->load(sid(ended)).has_value());
}

TEST(SessionFileStore, PayloadCapDoesNotConsumeSequence) {
    TempWorkspace workspace;
    auto          config = workspace.persistence();
    config.max_payload_bytes = 128;
    const std::string id = fresh_uuid();

    auto store = SessionPersistence::open(config);
    store->create(root_header(workspace.root, id));
    const Event oversized = event_for(
        id, EventType::UserMessage, nlohmann::json{{"x", std::string(256, 'x')}});
    EXPECT_THROW(store->append(sid(id), oversized), PayloadTooLarge);
    EXPECT_EQ(store->headSequence(sid(id)), 0);
    EXPECT_EQ(store->append(sid(id), event_for(id, EventType::SessionStarted, started_payload())), 1);
}

TEST(SessionFileStore, LeaseLifecycle) {
    TempWorkspace workspace;
    const auto    config = workspace.persistence();
    const std::string id = fresh_uuid();

    auto store = SessionPersistence::open(config);
    store->create(root_header(workspace.root, id));
    EXPECT_TRUE(store->isLeaseHolder(sid(id)));
    EXPECT_EQ(store->leaseState(sid(id)), ymh::LeaseState::HeldByMe);

    EXPECT_TRUE(store->releaseLease(sid(id)));
    EXPECT_FALSE(store->isLeaseHolder(sid(id)));
    EXPECT_EQ(store->leaseState(sid(id)), ymh::LeaseState::Absent);
    EXPECT_THROW(store->append(sid(id), event_for(id, EventType::SessionStarted, started_payload())),
                 ymh::LeaseLost);

    EXPECT_TRUE(store->acquireLease(sid(id)));
    EXPECT_TRUE(store->isLeaseHolder(sid(id)));
    EXPECT_EQ(store->append(sid(id), event_for(id, EventType::SessionStarted, started_payload())), 1);
}

TEST(SessionFileStore, ReadOnlyOpenDoesNotCreateState) {
    TempWorkspace workspace;
    const auto    config = workspace.persistence();

    auto store = SessionPersistence::openReadOnly(config);
    EXPECT_TRUE(store->list().empty());
    EXPECT_FALSE(store->load(SessionId{fresh_uuid()}).has_value());
    EXPECT_THROW(store->read(SessionId{fresh_uuid()}), ymh::UnknownSession);
    EXPECT_FALSE(std::filesystem::exists(workspace.root / ".ymh" / "sessions"));
}

TEST(SessionFileStore, MigratesLegacyDatabaseAndPreservesSequences) {
    TempWorkspace workspace;
    const auto    config = workspace.persistence();
    std::filesystem::create_directories(workspace.root / ".ymh");

    const std::string id = fresh_uuid();
    sqlite3*          db = nullptr;
    ASSERT_EQ(sqlite3_open(config.db_path.c_str(), &db), SQLITE_OK);
    auto exec = [&](const char* sql) {
        char* message = nullptr;
        ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &message), SQLITE_OK)
            << (message != nullptr ? message : "");
        sqlite3_free(message);
    };
    exec(R"sql(
        CREATE TABLE sessions (
            id TEXT PRIMARY KEY, cwd TEXT NOT NULL, created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL, title TEXT NOT NULL DEFAULT '',
            model TEXT NOT NULL DEFAULT '', server_profile TEXT NOT NULL DEFAULT 'interactive',
            kind TEXT NOT NULL DEFAULT 'root', parent_session TEXT, seed_length INTEGER,
            metadata JSON
        );
        CREATE TABLE events (
            sequence INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL,
            event_id TEXT NOT NULL UNIQUE, timestamp INTEGER NOT NULL,
            type TEXT NOT NULL, payload JSON NOT NULL
        );
        PRAGMA application_id = 0x594D4801;
        PRAGMA user_version = 1;
    )sql");

    auto bind_text = [](sqlite3_stmt* statement, int index, const std::string& value) {
        ASSERT_EQ(sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT),
                  SQLITE_OK);
    };
    auto bind_int = [](sqlite3_stmt* statement, int index, std::int64_t value) {
        ASSERT_EQ(sqlite3_bind_int64(statement, index, value), SQLITE_OK);
    };

    sqlite3_stmt* insert_session = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
                  db,
                  "INSERT INTO sessions(id,cwd,created_at,updated_at,title,model,"
                  "server_profile,kind,parent_session,seed_length,metadata) "
                  "VALUES(?,?,?,?,?,?,?,?,NULL,NULL,NULL)",
                  -1, &insert_session, nullptr),
              SQLITE_OK);
    bind_text(insert_session, 1, id);
    bind_text(insert_session, 2, workspace.root.string());
    bind_int(insert_session, 3, 1000);
    bind_int(insert_session, 4, 2000);
    bind_text(insert_session, 5, "migrated title");
    bind_text(insert_session, 6, "test-model");
    bind_text(insert_session, 7, "interactive");
    bind_text(insert_session, 8, "root");
    ASSERT_EQ(sqlite3_step(insert_session), SQLITE_DONE);
    sqlite3_finalize(insert_session);

    auto insert_event = [&](std::int64_t sequence, const std::string& type,
                            const std::string& payload) {
        sqlite3_stmt* statement = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(
                      db,
                      "INSERT INTO events(sequence,session_id,event_id,timestamp,type,payload) "
                      "VALUES(?,?,?,?,?,?)",
                      -1, &statement, nullptr),
                  SQLITE_OK);
        bind_int(statement, 1, sequence);
        bind_text(statement, 2, id);
        bind_text(statement, 3, fresh_uuid());
        bind_int(statement, 4, sequence * 10);
        bind_text(statement, 5, type);
        bind_text(statement, 6, payload);
        ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
        sqlite3_finalize(statement);
    };
    insert_event(10, "session/start", started_payload().dump());
    insert_event(11, "user/message", user_payload("hello").dump());
    sqlite3_close(db);

    auto store = SessionPersistence::open(config);
    ASSERT_EQ(store->list().size(), 1U);
    const std::optional<SessionHeader> header = store->load(sid(id));
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->title, "migrated title");
    const ymh::EventRange imported = store->read(sid(id));
    ASSERT_EQ(imported.size(), 2U);
    EXPECT_EQ(imported[0].seq, 10);
    EXPECT_EQ(imported[1].seq, 11);
    EXPECT_EQ(store->headSequence(sid(id)), 11);

    EXPECT_TRUE(store->acquireLease(sid(id)));
    EXPECT_EQ(store->append(sid(id), event_for(id, EventType::TurnStarted,
                                          nlohmann::json{{"turn", 1}, {"origin", "user"}})),
              12);

    EXPECT_TRUE(std::filesystem::is_regular_file(workspace.root / ".ymh" / "sessions.db.migrated"));
    EXPECT_FALSE(std::filesystem::exists(workspace.root / ".ymh" / "sessions.db"));
    EXPECT_TRUE(std::filesystem::is_regular_file(workspace.root / ".ymh" / "sessions" /
                                                 "layout-v1"));

    std::ifstream header_file{workspace.root / ".ymh" / "sessions" / id / "header.json"};
    ASSERT_TRUE(header_file.good());
    const nlohmann::json envelope = nlohmann::json::parse(header_file);
    EXPECT_EQ(envelope.at("base_sequence").get<ymh::Sequence>(), 11);
}

} // namespace

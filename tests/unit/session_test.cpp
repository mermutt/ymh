#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <unistd.h>

#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace {

using namespace ymh;

std::filesystem::path make_temp_dir() {
    const auto base = std::filesystem::temp_directory_path() /
                      ("ymh_session_" + std::to_string(::getpid()) + "_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

class FakeStore final : public SessionStore {
public:
    SessionHeader create(SessionHeader header) override {
        headers_[header.id.value] = header;
        logs_[header.id.value]    = {};
        return header;
    }

    std::optional<SessionHeader> load(SessionId id) const override {
        const auto it = headers_.find(id.value);
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<SessionHeader> list() const override {
        std::vector<SessionHeader> result;
        for (const auto& [key, header] : headers_) {
            result.push_back(header);
        }
        return result;
    }

    void erase(SessionId id) override { headers_.erase(id.value); }

    EventRange read(SessionId id, Sequence after = 0) const override {
        EventRange result;
        for (const EventRecord& record : resolve(id)) {
            if (record.seq > after) {
                result.push_back(record);
            }
        }
        return result;
    }

    EventRange readRange(SessionId id, Sequence from, Sequence to) const override {
        EventRange result;
        for (const EventRecord& record : resolve(id)) {
            if (record.seq >= from && record.seq <= to) {
                result.push_back(record);
            }
        }
        return result;
    }

    Sequence append(SessionId id, Event event) override {
        if (!lease) {
            throw LeaseLost("fake store is not the lease holder");
        }
        const Sequence seq = ++global_;
        logs_[id.value].push_back(EventRecord{seq, std::move(event)});
        return seq;
    }

    bool isLeaseHolder(SessionId) const override { return lease; }

    bool lease = true;

private:
    EventRange resolve(const SessionId& id) const {
        const auto header = load(id);
        if (!header.has_value()) {
            throw UnknownSession("unknown session");
        }
        EventRange own = logs_.at(id.value);
        if (header->kind != SessionKind::Fork || !header->parentSession.has_value()) {
            return own;
        }
        const EventRange parent = resolve(*header->parentSession);
        const std::size_t seed  = header->seedLength.value_or(0);
        EventRange result(parent.begin(),
                          parent.begin() + static_cast<std::ptrdiff_t>(seed));
        result.insert(result.end(), own.begin(), own.end());
        return result;
    }

    std::unordered_map<std::string, SessionHeader> headers_;
    std::unordered_map<std::string, EventRange>    logs_;
    Sequence                                       global_ = 0;
};

SessionHeader make_header(const std::filesystem::path& cwd, SessionKind kind = SessionKind::Root) {
    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = cwd;
    header.createdAt     = 1000;
    header.updatedAt     = 1000;
    header.model         = "test-model";
    header.serverProfile = "interactive";
    header.kind          = kind;
    return header;
}

template <class P>
EventRecord record(Sequence seq, const SessionId& session, const P& payload) {
    TypedEvent<P> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::time_point{std::chrono::milliseconds{seq * 10}};
    typed.payload    = payload;
    return EventRecord{seq, encode(typed)};
}

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

ContentBlock tool_use_block(std::string id) {
    ContentBlock block;
    block.kind         = ContentBlockKind::ToolUse;
    block.tool_call_id = std::move(id);
    block.tool_name    = "read";
    return block;
}

TEST(ValidateHeader, KindParentSeedMatrix) {
    const auto cwd = make_temp_dir();

    SessionHeader root = make_header(cwd);
    EXPECT_NO_THROW(validateHeader(root));

    root.parentSession = make_session_id();
    EXPECT_THROW(validateHeader(root), std::invalid_argument);

    SessionHeader fork = make_header(cwd, SessionKind::Fork);
    EXPECT_THROW(validateHeader(fork), std::invalid_argument);
    fork.parentSession = make_session_id();
    fork.seedLength    = 0;
    EXPECT_NO_THROW(validateHeader(fork));

    SessionHeader subagent = make_header(cwd, SessionKind::Subagent);
    EXPECT_THROW(validateHeader(subagent), std::invalid_argument);
    subagent.parentSession = make_session_id();
    EXPECT_NO_THROW(validateHeader(subagent));
    subagent.seedLength = 0;
    EXPECT_NO_THROW(validateHeader(subagent));
    subagent.seedLength = 3;
    EXPECT_THROW(validateHeader(subagent), std::invalid_argument);
}

TEST(ValidateHeader, RejectsMalformedIdentityAndCwd) {
    const auto cwd = make_temp_dir();

    SessionHeader header = make_header(cwd);
    header.id.value      = "not-a-uuid";
    EXPECT_THROW(validateHeader(header), std::invalid_argument);

    SessionHeader relative = make_header(cwd);
    relative.cwd           = "relative/path";
    EXPECT_THROW(validateHeader(relative), std::invalid_argument);

    EXPECT_THROW(validateWorkspaceRoot(cwd / "missing"), std::invalid_argument);
}

TEST(SessionHeaderJson, RoundTripPreservesFields) {
    const auto cwd = make_temp_dir();
    SessionHeader header = make_header(cwd, SessionKind::Fork);
    header.parentSession = make_session_id();
    header.seedLength    = 7;
    header.title         = "title";
    header.metadata      = R"({"k":"v"})";

    const nlohmann::json json = header;
    const SessionHeader  back = json.get<SessionHeader>();
    EXPECT_EQ(header, back);

    EXPECT_FALSE(json.contains("boot_id"));
    EXPECT_FALSE(json.contains("boot_nonce"));
    EXPECT_FALSE(json.contains("ordinal"));
    EXPECT_FALSE(json.contains("archived"));
}

TEST(DeriveMessages, ProjectsUserAssistantToolAndInjected) {
    const SessionId session = make_session_id();

    payload::UserMessage user;
    user.id      = "m1";
    user.content = {text_block("hello")};

    payload::AssistantMessage assistant;
    assistant.id      = "m2";
    assistant.content = {tool_use_block("call-1")};

    payload::ToolResult result;
    result.id      = "call-1";
    result.name    = "read";
    result.outcome = payload::ToolOutcome::Ok;
    result.output  = "file contents";

    payload::ContextInjected injected;
    injected.id   = "m3";
    injected.role = Role::System;
    injected.text = "context";

    const EventRange events{
        record(1, session, user),
        record(2, session, assistant),
        record(3, session, result),
        record(4, session, injected),
    };

    const std::vector<Message> messages = deriveMessages(SessionHeader{}, events);
    ASSERT_EQ(messages.size(), 4u);
    EXPECT_EQ(messages[0].role, Role::User);
    EXPECT_EQ(messages[0].content.front().text, "hello");
    EXPECT_EQ(messages[1].role, Role::Assistant);
    EXPECT_EQ(messages[2].role, Role::Tool);
    EXPECT_EQ(messages[2].tool_call_id, "call-1");
    EXPECT_EQ(messages[2].content.front().text, "file contents");
    EXPECT_EQ(messages[3].role, Role::System);
    EXPECT_EQ(messages[3].content.front().text, "context");
}

TEST(DeriveMessages, CancelledTurnSynthesizesCancelledToolResult) {
    const SessionId session = make_session_id();

    payload::TurnStarted started;
    started.turn   = 1;
    started.origin = payload::TurnOrigin::User;

    payload::AssistantMessage assistant;
    assistant.id      = "m1";
    assistant.content = {tool_use_block("call-1"), tool_use_block("call-2")};

    payload::ToolResult partial;
    partial.id      = "call-1";
    partial.name    = "read";
    partial.outcome = payload::ToolOutcome::Ok;
    partial.output  = "ok";

    payload::TurnCancelled cancelled;
    cancelled.turn   = 1;
    cancelled.reason = "user";

    const EventRange events{
        record(1, session, started),
        record(2, session, assistant),
        record(3, session, partial),
        record(4, session, cancelled),
    };

    const std::vector<Message> messages = deriveMessages(SessionHeader{}, events);
    ASSERT_EQ(messages.size(), 3u);
    EXPECT_EQ(messages[1].tool_call_id, "call-1");
    EXPECT_EQ(messages[2].role, Role::Tool);
    EXPECT_EQ(messages[2].tool_call_id, "call-2");
    EXPECT_NE(messages[2].content.front().text.find("cancel"), std::string::npos);
}

TEST(DeriveMessages, FailedTurnSynthesizesErrorToolResult) {
    const SessionId session = make_session_id();

    payload::TurnStarted started;
    started.turn   = 1;
    started.origin = payload::TurnOrigin::User;

    payload::AssistantMessage assistant;
    assistant.id      = "m1";
    assistant.content = {tool_use_block("call-1")};

    payload::TurnFailed failed;
    failed.turn    = 1;
    failed.code    = "RateLimited";
    failed.message = "provider failed";

    const EventRange events{
        record(1, session, started),
        record(2, session, assistant),
        record(3, session, failed),
    };

    const std::vector<Message> messages = deriveMessages(SessionHeader{}, events);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[1].role, Role::Tool);
    EXPECT_EQ(messages[1].tool_call_id, "call-1");
    EXPECT_NE(messages[1].content.front().text.find("fail"), std::string::npos);
}

TEST(DeriveMessages, ChunksAndToolCallsAreNotProjected) {
    const SessionId session = make_session_id();

    payload::AssistantChunk chunk;
    chunk.message = "m1";
    chunk.index   = 0;
    chunk.text    = "delta";

    payload::ToolCall call;
    call.id   = "call-1";
    call.turn = 1;
    call.step = 1;
    call.name = "read";

    const EventRange events{record(1, session, chunk), record(2, session, call)};
    EXPECT_TRUE(deriveMessages(SessionHeader{}, events).empty());
}

TEST(DeriveMessages, CompactionFoldsEarlierMessages) {
    const SessionId session = make_session_id();

    payload::UserMessage user;
    user.id      = "m1";
    user.content = {text_block("one")};

    payload::ContextCompaction compaction;
    compaction.boundary      = 1;
    compaction.summary       = "summary";
    compaction.tokenEstimate = 10;
    compaction.model         = "test-model";

    const EventRange events{record(1, session, user), record(2, session, compaction)};
    const std::vector<Message> messages = deriveMessages(SessionHeader{}, events);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].role, Role::System);
    EXPECT_EQ(messages[0].content.front().text, "summary");
}

TEST(DeriveMessages, IsDeterministic) {
    const SessionId session = make_session_id();
    payload::UserMessage user;
    user.id      = "m1";
    user.content = {text_block("hello")};
    const EventRange events{record(1, session, user)};

    EXPECT_TRUE(deriveMessages(SessionHeader{}, events).size() ==
                deriveMessages(SessionHeader{}, events).size());
    const auto first  = deriveMessages(SessionHeader{}, events);
    const auto second = deriveMessages(SessionHeader{}, events);
    ASSERT_EQ(first.size(), second.size());
    EXPECT_EQ(first[0].content.front().text, second[0].content.front().text);
}

TEST(Session, AppendIncreasesSequenceAndTracksUpdatedAt) {
    const auto   cwd = make_temp_dir();
    FakeStore    store;
    EventBus     bus;
    SessionHeader header = make_header(cwd);

    store.create(header);
    Session session(header, store, bus);
    const Sequence first  = session.append(payload::SessionStarted{"m", "interactive", "t"});
    const Sequence second = session.append(payload::TurnStarted{1, payload::TurnOrigin::User});
    EXPECT_LT(first, second);

    const EventRange events = session.events();
    ASSERT_EQ(events.size(), 2u);
    const auto expected = std::chrono::duration_cast<std::chrono::milliseconds>(
                              events.back().event.timestamp.time_since_epoch())
                              .count();
    EXPECT_EQ(session.header().updatedAt, expected);
    EXPECT_EQ(session.nextTurnId(), 2u);
}

TEST(Session, PersistBeforePublish) {
    const auto   cwd = make_temp_dir();
    FakeStore    store;
    EventBus     bus;
    SessionHeader header = make_header(cwd);
    store.create(header);
    Session      session(header, store, bus);

    int observed = 0;
    auto subscription = bus.subscribe([&](const Event& event) {
        if (event.type == EventType::UserMessage) {
            ++observed;
        }
    });

    payload::UserMessage user;
    user.id      = "m1";
    user.content = {text_block("hi")};
    session.append(user);

    EXPECT_EQ(observed, 1);
    EXPECT_EQ(session.events().size(), 1u);
}

TEST(Session, NonHolderAppendThrowsLeaseLost) {
    const auto   cwd = make_temp_dir();
    FakeStore    store;
    EventBus     bus;
    SessionHeader header = make_header(cwd);
    store.create(header);
    Session      session(header, store, bus);

    store.lease = false;
    EXPECT_THROW(session.append(payload::SessionStarted{}), LeaseLost);
    EXPECT_TRUE(session.events().empty());
}

TEST(Session, SnapshotMatchesDerivedMessages) {
    const auto   cwd = make_temp_dir();
    FakeStore    store;
    EventBus     bus;
    SessionHeader header = make_header(cwd);
    store.create(header);
    Session      session(header, store, bus);

    payload::UserMessage user;
    user.id      = "m1";
    user.content = {text_block("hi")};
    session.append(user);

    const SessionSnapshot snapshot = session.snapshot();
    ASSERT_EQ(snapshot.messages.size(), session.deriveMessages().size());
    EXPECT_EQ(snapshot.messages.size(), 1u);
    EXPECT_EQ(snapshot.at, session.events().back().seq);
}

TEST(SessionManager, CreateResumeForkReplay) {
    const auto   cwd = make_temp_dir();
    FakeStore    store;
    EventBus     bus;
    SessionManager manager(store, bus);

    SessionOptions options;
    options.cwd           = cwd;
    options.serverProfile = "interactive";
    options.model         = "test-model";
    options.title         = "session";

    const SessionId root = manager.createSession(options);
    Session&        rootSession = manager.session(root);
    EXPECT_EQ(rootSession.kind(), SessionKind::Root);
    ASSERT_EQ(rootSession.events().size(), 1u);

    rootSession.append(payload::TurnStarted{1, payload::TurnOrigin::User});
    rootSession.append(payload::TurnEnded{1});

    const SessionId forked = manager.forkSession(root, 2);
    Session&        forkSession = manager.session(forked);
    EXPECT_EQ(forkSession.kind(), SessionKind::Fork);
    ASSERT_EQ(forkSession.events().size(), 3u);
    EXPECT_EQ(forkSession.events()[0].seq, rootSession.events()[0].seq);
    EXPECT_EQ(forkSession.events()[1].seq, rootSession.events()[1].seq);
    EXPECT_GT(forkSession.events()[2].seq, forkSession.events()[1].seq);

    const SessionId resumed = manager.resumeSession(root);
    EXPECT_EQ(resumed.value, root.value);

    const SessionId replayed = manager.replaySession(forked);
    EXPECT_EQ(replayed.value, forked.value);
    EXPECT_THROW(manager.replaySession(SessionId{"00000000-0000-4000-8000-000000000000"}),
                 UnknownSession);
}

TEST(SessionManager, DeleteEmitsSessionEnded) {
    const auto   cwd = make_temp_dir();
    FakeStore    store;
    EventBus     bus;
    SessionManager manager(store, bus);

    SessionOptions options;
    options.cwd           = cwd;
    options.serverProfile = "interactive";
    options.model         = "test-model";

    const SessionId id = manager.createSession(options);
    manager.deleteSession(id);

    EXPECT_FALSE(store.load(id).has_value());
    EXPECT_THROW(manager.session(id), UnknownSession);
}

TEST(SessionStoreDefaults, FakeStoreHeadSequenceAndBoundedReadAfter) {
    FakeStore          store;
    const SessionHeader header = make_header(make_temp_dir());
    store.create(header);
    EXPECT_EQ(store.headSequence(header.id), 0);

    const Sequence first = store.append(
        header.id,
        record(1, header.id, payload::SessionStarted{"test-model", "interactive", "t"}).event);
    const Sequence second = store.append(
        header.id,
        record(2, header.id, payload::SessionStarted{"test-model", "interactive", "t"}).event);

    EXPECT_EQ(store.headSequence(header.id), second);
    EXPECT_TRUE(store.readAfter(header.id, 0, 0).empty());

    const EventRange bounded = store.readAfter(header.id, 0, 1);
    ASSERT_EQ(bounded.size(), 1u);
    EXPECT_EQ(bounded[0].seq, first);
    EXPECT_EQ(store.readAfter(header.id, first, kUnbounded).size(), 1u);
    EXPECT_THROW(static_cast<void>(store.headSequence(SessionId{"missing"})), UnknownSession);
}

TEST(SessionRename, PayloadJsonRoundTripAndOriginParsing) {
    const payload::SessionRenamed user{"fix the flaky test", payload::RenameOrigin::User};
    const nlohmann::json         json = user;
    EXPECT_EQ(json.at("origin").get<std::string>(), "user");
    const payload::SessionRenamed restored = json.get<payload::SessionRenamed>();
    EXPECT_EQ(restored.title, user.title);
    EXPECT_EQ(restored.origin, payload::RenameOrigin::User);

    nlohmann::json auto_json = user;
    auto_json["origin"]      = "auto";
    EXPECT_EQ(auto_json.get<payload::SessionRenamed>().origin, payload::RenameOrigin::Auto);

    nlohmann::json bogus = user;
    bogus["origin"]      = "bogus";
    EXPECT_THROW(static_cast<void>(bogus.get<payload::SessionRenamed>()), std::runtime_error);
}

TEST(SessionRename, WireNameIsPinned) {
    EXPECT_EQ(wire_name(EventType::SessionRenamed), "session/renamed");
    const std::optional<EventType> parsed = parse_event_type("session/renamed");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, EventType::SessionRenamed);
}

TEST(SessionRename, DeriveMessagesIgnoresRename) {
    const SessionId session = make_session_id();
    const EventRange before  = {
        record(1, session, payload::SessionStarted{"m", "interactive", "t"}),
        record(2, session, payload::UserMessage{MessageId{"m1"}, {text_block("hi")}}),
    };
    EventRange with_rename = before;
    with_rename.push_back(
        record(3, session, payload::SessionRenamed{"renamed", payload::RenameOrigin::User}));

    SessionHeader header = make_header(make_temp_dir());
    header.id            = session;
    const std::vector<Message> plain  = deriveMessages(header, before);
    const std::vector<Message> renamed = deriveMessages(header, with_rename);
    ASSERT_EQ(plain.size(), renamed.size());
    for (std::size_t index = 0; index < plain.size(); ++index) {
        EXPECT_EQ(plain[index].role, renamed[index].role);
        ASSERT_EQ(plain[index].content.size(), renamed[index].content.size());
        for (std::size_t block = 0; block < plain[index].content.size(); ++block) {
            EXPECT_EQ(plain[index].content[block].text, renamed[index].content[block].text);
        }
    }
}

TEST(SessionTitle, PlaceholderSet) {
    EXPECT_TRUE(is_placeholder_title(""));
    EXPECT_TRUE(is_placeholder_title("tui"));
    EXPECT_TRUE(is_placeholder_title("main"));
    EXPECT_FALSE(is_placeholder_title("my title"));
    EXPECT_FALSE(is_placeholder_title("tui2"));
}

TEST(SessionTitle, NormalizeTrimsOnlyThePinnedSet) {
    EXPECT_EQ(normalize_title("  my title  "), "my title");
    EXPECT_EQ(normalize_title("\tmy title\t"), "my title");
    EXPECT_EQ(normalize_title("\rmy title\f"), "my title");
    EXPECT_EQ(normalize_title("\vmy title\v"), "my title");
    EXPECT_EQ(normalize_title("a  b"), "a  b");
    EXPECT_THROW(static_cast<void>(normalize_title("")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("   ")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("\t\r\v\f")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("\nmy title")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("my title\n")), std::invalid_argument);
}

TEST(SessionTitle, NormalizeRejectsControlsDelAndInvalidUtf8) {
    EXPECT_THROW(static_cast<void>(normalize_title("a\x01" "b")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("a\x7f" "b")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("\xc0\xaf")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("\xed\xa0\x80")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("\xf4\x90\x80\x80")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("\xe2\x82")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(normalize_title("\x80")), std::invalid_argument);

    EXPECT_EQ(normalize_title("\xc3\xa9"), "\xc3\xa9");
    EXPECT_EQ(normalize_title("\xe2\x82\xac"), "\xe2\x82\xac");
    EXPECT_EQ(normalize_title("\xf0\x9f\x98\x80"), "\xf0\x9f\x98\x80");
}

TEST(SessionTitle, NormalizeLengthIsBytesRejectedNotTruncated) {
    const std::string exactly_120(120, 'a');
    EXPECT_EQ(normalize_title(exactly_120), exactly_120);
    EXPECT_THROW(static_cast<void>(normalize_title(std::string(121, 'a'))),
                 std::invalid_argument);
}

TEST(SessionTitle, DeriveAutoTitleFirstLineTrimCollapse) {
    ASSERT_TRUE(derive_auto_title("hello\nworld").has_value());
    EXPECT_EQ(*derive_auto_title("hello\nworld"), "hello");
    EXPECT_EQ(*derive_auto_title("  spaced \t out  "), "spaced out");
    EXPECT_EQ(*derive_auto_title("no newline here"), "no newline here");
    EXPECT_FALSE(derive_auto_title("").has_value());
    EXPECT_FALSE(derive_auto_title("   ").has_value());
    EXPECT_FALSE(derive_auto_title("\nleading newline").has_value());
    EXPECT_FALSE(derive_auto_title("bad\x01text").has_value());
    EXPECT_FALSE(derive_auto_title("\xc0\xaf").has_value());
}

TEST(SessionTitle, DeriveAutoTitleCutsOnCodePointBoundary) {
    const std::string long_ascii(100, 'a');
    const std::optional<std::string> ascii = derive_auto_title(long_ascii);
    ASSERT_TRUE(ascii.has_value());
    EXPECT_EQ(ascii->size(), 63u);
    EXPECT_EQ(ascii->substr(ascii->size() - 3), "...");
    EXPECT_EQ(ascii->substr(0, 60), std::string(60, 'a'));

    // 1 + 30 * 3-byte code points: byte 60 falls inside the 20th code point.
    std::string multibyte = "a";
    for (int index = 0; index < 30; ++index) {
        multibyte += "\xe2\x82\xac";
    }
    const std::optional<std::string> cut = derive_auto_title(multibyte);
    ASSERT_TRUE(cut.has_value());
    EXPECT_EQ(cut->size(), 61u);
    EXPECT_EQ(cut->substr(cut->size() - 3), "...");
    EXPECT_NO_THROW(static_cast<void>(normalize_title(*cut)));
}

TEST(SessionManagerRename, AppendsUserRenameAndUpdatesHeader) {
    FakeStore      store;
    EventBus       bus;
    SessionManager manager(store, bus);
    SessionOptions options;
    options.cwd           = make_temp_dir();
    options.serverProfile = "interactive";
    options.model         = "test-model";
    options.title         = "tui";
    const SessionId id    = manager.createSession(options);

    const Sequence sequence = manager.renameSession(id, "  my  title  ");
    EXPECT_GT(sequence, 0);
    EXPECT_EQ(manager.session(id).header().title, "my  title");

    std::size_t renames = 0;
    for (const EventRecord& record : manager.session(id).ownEvents()) {
        if (record.event.type != EventType::SessionRenamed) {
            continue;
        }
        ++renames;
        const auto payload = record.event.payload.get<payload::SessionRenamed>();
        EXPECT_EQ(payload.title, "my  title");
        EXPECT_EQ(payload.origin, payload::RenameOrigin::User);
    }
    EXPECT_EQ(renames, 1u);
}

TEST(SessionManagerRename, UnknownIdAndBadTitle) {
    FakeStore      store;
    EventBus       bus;
    SessionManager manager(store, bus);
    SessionOptions options;
    options.cwd           = make_temp_dir();
    options.serverProfile = "interactive";
    options.model         = "test-model";
    options.title         = "tui";
    const SessionId id    = manager.createSession(options);

    EXPECT_THROW(manager.renameSession(SessionId{"missing"}, "x"), UnknownSession);
    EXPECT_THROW(manager.renameSession(id, "   "), std::invalid_argument);

    std::size_t renames = 0;
    for (const EventRecord& record : manager.session(id).ownEvents()) {
        if (record.event.type == EventType::SessionRenamed) {
            ++renames;
        }
    }
    EXPECT_EQ(renames, 0u);
}

TEST(SessionManagerAutoName, FiresOnceWithDerivedTitle) {
    FakeStore      store;
    EventBus       bus;
    SessionManager manager(store, bus);
    SessionOptions options;
    options.cwd           = make_temp_dir();
    options.serverProfile = "interactive";
    options.model         = "test-model";
    options.title         = "tui";
    const SessionId id    = manager.createSession(options);

    const std::optional<Sequence> first = manager.maybeAutoName(id, "fix the flaky PTY test");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(manager.session(id).header().title, "fix the flaky PTY test");

    const std::optional<Sequence> second = manager.maybeAutoName(id, "second prompt");
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(manager.session(id).header().title, "fix the flaky PTY test");

    std::size_t renames = 0;
    for (const EventRecord& record : manager.session(id).ownEvents()) {
        if (record.event.type != EventType::SessionRenamed) {
            continue;
        }
        ++renames;
        EXPECT_EQ(record.event.payload.get<payload::SessionRenamed>().origin,
                  payload::RenameOrigin::Auto);
    }
    EXPECT_EQ(renames, 1u);
}

TEST(SessionManagerAutoName, ManualRenameIsNeverOverwritten) {
    FakeStore      store;
    EventBus       bus;
    SessionManager manager(store, bus);
    SessionOptions options;
    options.cwd           = make_temp_dir();
    options.serverProfile = "interactive";
    options.model         = "test-model";
    options.title         = "tui";
    const SessionId id    = manager.createSession(options);

    manager.renameSession(id, "manual name");
    const std::optional<Sequence> suppressed = manager.maybeAutoName(id, "first prompt");
    EXPECT_FALSE(suppressed.has_value());
    EXPECT_EQ(manager.session(id).header().title, "manual name");

    std::size_t renames = 0;
    for (const EventRecord& record : manager.session(id).ownEvents()) {
        if (record.event.type != EventType::SessionRenamed) {
            continue;
        }
        ++renames;
        EXPECT_EQ(record.event.payload.get<payload::SessionRenamed>().origin,
                  payload::RenameOrigin::User);
    }
    EXPECT_EQ(renames, 1u);
}

TEST(SessionManagerAutoName, NonPlaceholderAndEmptyPromptSuppress) {
    FakeStore      store;
    EventBus       bus;
    SessionManager manager(store, bus);
    SessionOptions options;
    options.cwd           = make_temp_dir();
    options.serverProfile = "interactive";
    options.model         = "test-model";
    options.title         = "headless-style";
    const SessionId id    = manager.createSession(options);

    EXPECT_FALSE(manager.maybeAutoName(id, "prompt").has_value());

    FakeStore      empty_store;
    EventBus       empty_bus;
    SessionManager empty_manager(empty_store, empty_bus);
    options.title = "";
    const SessionId empty_id = empty_manager.createSession(options);
    EXPECT_FALSE(empty_manager.maybeAutoName(empty_id, "").has_value());
    EXPECT_FALSE(empty_manager.maybeAutoName(empty_id, "   \n  ").has_value());
    EXPECT_FALSE(empty_manager.maybeAutoName(SessionId{"missing"}, "prompt").has_value());
}

TEST(SessionAutoName, ConcurrentAppendAndAutoNameAppendsExactlyOnce) {
    FakeStore    store;
    EventBus     bus;
    SessionHeader header = make_header(make_temp_dir());
    header.title         = "tui";
    store.create(header);
    Session session(header, store, bus);

    std::atomic<bool> start{false};
    std::thread worker([&session, &start] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int index = 0; index < 200; ++index) {
            session.append(payload::ContextInjected{MessageId{"m" + std::to_string(index)},
                                                    Role::System, "x"});
        }
    });
    std::thread io([&session, &start] {
        while (!start.load(std::memory_order_acquire)) {
        }
        static_cast<void>(session.appendAutoRename("fix the flaky PTY test"));
    });
    start.store(true, std::memory_order_release);
    worker.join();
    io.join();

    std::size_t renames = 0;
    for (const EventRecord& record : session.ownEvents()) {
        if (record.event.type == EventType::SessionRenamed) {
            ++renames;
        }
    }
    EXPECT_EQ(renames, 1u);
    EXPECT_EQ(session.header().title, "fix the flaky PTY test");
}

} // namespace

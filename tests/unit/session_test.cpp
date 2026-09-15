#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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

} // namespace

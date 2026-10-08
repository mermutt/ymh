#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/handoff.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

std::chrono::system_clock::time_point fixed_clock() {
    return std::chrono::system_clock::time_point{std::chrono::seconds{1'600'000'000}};
}

const std::vector<std::string> kHeadings = {"## Goal", "## Key Decisions", "## Current State",
                                            "## Next Steps", "## Open Questions",
                                            "## Relevant Files"};

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

std::string summary_text() {
    std::string text;
    for (const std::string& heading : kHeadings) {
        text += heading + "\n- done\n";
    }
    return text;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream      input{path, std::ios::binary};
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

HandoffPolicy base_policy() {
    HandoffPolicy policy;
    policy.enabled            = true;
    policy.max_summary_tokens = 2048;
    policy.max_summary_bytes  = 256u * 1024u;
    policy.max_input_bytes    = 256u * 1024u;
    policy.keep_recent_turns  = 2;
    return policy;
}

struct Fixture {
    EventBus           bus;
    MemorySessionStore store;
    SessionManager     sessions{store, bus};
    TempWorkspace      workspace{"handoff_seed"};
    LocalEnvironment   env{workspace.path()};
    ProviderRuntime    provider{FakeScript{{text_step(summary_text())}}};
    LLMPool            pool{1};
    HandoffPolicy      policy = base_policy();

    SessionId make_source() {
        SessionOptions options;
        options.cwd           = workspace.path();
        options.serverProfile = "interactive";
        options.model         = "fake-model";
        options.title         = "source";
        options.kind          = SessionKind::Root;
        return sessions.createSession(options);
    }
};

void append_turn(Session& session, TurnId turn, const std::string& user,
                 const std::string& assistant) {
    session.append(payload::TurnStarted{turn, payload::TurnOrigin::User});
    payload::UserMessage user_message;
    user_message.id = make_event_id().value;
    user_message.content.push_back(text_block(user));
    session.append(user_message);
    payload::AssistantMessage assistant_message;
    assistant_message.id = make_event_id().value;
    assistant_message.content.push_back(text_block(assistant));
    session.append(assistant_message);
    session.append(payload::TurnEnded{turn});
}

std::optional<payload::ContextInjected> first_injected(const EventRange& events) {
    for (const EventRecord& record : events) {
        if (record.event.type == EventType::ContextInjected) {
            return record.event.payload.get<payload::ContextInjected>();
        }
    }
    return std::nullopt;
}

} // namespace

TEST(HandoffSeedTest, HS_U11_SeedIsAStoredOnlyRoot) {
    Fixture fixture;
    const SessionId source = fixture.make_source();
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "hello", "world");

    HandoffService    service(fixture.provider.runtime(), fixture.pool, fixture.sessions,
                              fixture.env, nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{true, {}, {}}, CancellationToken{});
    ASSERT_EQ(result.outcome, HandoffResult::Outcome::Ok);
    ASSERT_TRUE(result.seed_session.has_value());

    const std::optional<SessionHeader> header = fixture.store.load(*result.seed_session);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->kind, SessionKind::Root);
    EXPECT_FALSE(header->parentSession.has_value());
    EXPECT_FALSE(header->seedLength.has_value());
    EXPECT_EQ(header->title, "handoff: source");
    EXPECT_EQ(fixture.store.list().size(), 2u);
}

TEST(HandoffSeedTest, HS_U12_SeedFirstEventIsContextInjectedRecall) {
    Fixture fixture;
    const SessionId source = fixture.make_source();
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "hello", "world");

    HandoffService    service(fixture.provider.runtime(), fixture.pool, fixture.sessions,
                              fixture.env, nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{true, {}, {}}, CancellationToken{});
    ASSERT_EQ(result.outcome, HandoffResult::Outcome::Ok);
    ASSERT_TRUE(result.seed_session.has_value());

    const EventRange events = fixture.store.read(*result.seed_session);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.front().event.type, EventType::SessionStarted);
    const std::optional<payload::ContextInjected> injected = first_injected(events);
    ASSERT_TRUE(injected.has_value());
    EXPECT_EQ(injected->role, Role::User);
    EXPECT_EQ(injected->source.kind, MessageSource::Kind::Plugin);
    EXPECT_EQ(injected->source.plugin, "ymh.handoff");
    EXPECT_EQ(injected->source.context.form, ContextForm::Recall);

    const std::string doc = read_file(fixture.workspace.path() / result.doc_relative);
    EXPECT_NE(doc.find(injected->text), std::string::npos);
    for (const std::string& heading : kHeadings) {
        EXPECT_NE(injected->text.find(heading), std::string::npos);
    }
}

TEST(HandoffSeedTest, HS_U13_SeedReplayShowsSummaryAsFirstProjectedMessage) {
    Fixture fixture;
    const SessionId source = fixture.make_source();
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "hello", "world");

    HandoffService    service(fixture.provider.runtime(), fixture.pool, fixture.sessions,
                              fixture.env, nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{true, {}, {}}, CancellationToken{});
    ASSERT_EQ(result.outcome, HandoffResult::Outcome::Ok);
    ASSERT_TRUE(result.seed_session.has_value());

    fixture.sessions.resumeSession(*result.seed_session);
    std::shared_ptr<Session> seed = fixture.sessions.sessionPtr(*result.seed_session);
    const std::vector<Message> projected = seed->deriveMessages();
    ASSERT_FALSE(projected.empty());
    bool found = false;
    for (const Message& message : projected) {
        for (const ContentBlock& block : message.content) {
            if (block.text.find("## Goal") != std::string::npos) {
                found = true;
            }
        }
    }
    EXPECT_TRUE(found);
}

TEST(HandoffSeedTest, HS_U14_NoSeedWritesOnlyTheDoc) {
    Fixture fixture;
    const SessionId source = fixture.make_source();
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "hello", "world");

    HandoffService    service(fixture.provider.runtime(), fixture.pool, fixture.sessions,
                              fixture.env, nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{false, {}, {}}, CancellationToken{});
    EXPECT_EQ(result.outcome, HandoffResult::Outcome::Ok);
    EXPECT_FALSE(result.seed_session.has_value());
    EXPECT_EQ(fixture.store.list().size(), 1u);
    EXPECT_TRUE(std::filesystem::exists(fixture.workspace.path() / result.doc_relative));
}

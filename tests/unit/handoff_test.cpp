#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/handoff.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/stream.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

const std::vector<std::string> kHeadings = {"## Goal", "## Key Decisions", "## Current State",
                                            "## Next Steps", "## Open Questions",
                                            "## Relevant Files"};

std::chrono::system_clock::time_point fixed_clock() {
    return std::chrono::system_clock::time_point{std::chrono::seconds{1'600'000'000}};
}

constexpr std::time_t kStampTime = 1'600'000'000;

std::string stamp_of(std::time_t now) {
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return out.str();
}

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
    std::ifstream     input{path, std::ios::binary};
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

class RecordingProvider final : public LLMProvider {
public:
    ProviderId id() const override { return "recording"; }

    ProviderCapabilities capabilities() const override { return {}; }

    std::vector<ModelInfo> models() const override {
        return {ModelInfo{"fake-model", "Recording", 0}};
    }

    Task<LLMResponse> stream(const LLMRequest& request, StreamSink sink,
                             CancellationToken cancel) override {
        (void)cancel;
        last_messages = request.messages;
        for (const Message& message : request.messages) {
            if (message.role != Role::User) {
                continue;
            }
            last_user_text.clear();
            for (const ContentBlock& block : message.content) {
                if (block.kind == ContentBlockKind::Text) {
                    last_user_text += block.text;
                }
            }
        }
        LLMResponse response;
        response.outcome = StreamOutcome::Completed;
        response.finish  = FinishReason::Stop;
        sink(StreamEvent{TextDelta{summary_text()}});
        Finished finished;
        finished.reason = FinishReason::Stop;
        sink(StreamEvent{finished});
        return Task<LLMResponse>{std::move(response)};
    }

    std::vector<Message> last_messages;
    std::string          last_user_text;
};

struct Fixture {
    EventBus                bus;
    MemorySessionStore      store;
    SessionManager          sessions{store, bus};
    TempWorkspace           workspace{"handoff"};
    LocalEnvironment        env{workspace.path()};
    ProviderRuntime         provider{FakeScript{{text_step(summary_text())}}};
    LLMPool                 pool{1};
    HandoffPolicy           policy = base_policy();

    SessionId make_source(const std::string& title = "source",
                          const std::string& model = "fake-model") {
        SessionOptions options;
        options.cwd           = workspace.path();
        options.serverProfile = "interactive";
        options.model         = model;
        options.title         = title;
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

} // namespace

TEST(HandoffTest, HS_U1_InstructionHasSixSectionsInOrder) {
    const std::string_view instruction = handoff_instruction();
    std::size_t            cursor      = 0;
    for (const std::string& heading : kHeadings) {
        const std::size_t found = instruction.find(heading, cursor);
        ASSERT_NE(found, std::string_view::npos) << "missing " << heading;
        cursor = found;
    }
}

TEST(HandoffTest, HS_U1_MissingSectionIsNormalizedToNone) {
    Fixture fixture;
    ProviderRuntime custom{FakeScript{{text_step("## Goal\n- only goal\n")}}};
    const SessionId source = fixture.make_source();
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "hello", "world");

    HandoffService    service(custom.runtime(), fixture.pool, fixture.sessions, fixture.env,
                              nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{false, {}, {}}, CancellationToken{});
    ASSERT_EQ(result.outcome, HandoffResult::Outcome::Ok);

    const std::string doc = read_file(fixture.workspace.path() / result.doc_relative);
    std::size_t       cursor = 0;
    for (const std::string& heading : kHeadings) {
        const std::size_t found = doc.find(heading, cursor);
        ASSERT_NE(found, std::string::npos) << "missing " << heading;
        cursor = found;
    }
    EXPECT_NE(doc.find("(none)"), std::string::npos);
}

TEST(HandoffTest, HS_U2_EvidenceDigestIsDeterministic) {
    Fixture fixture;
    const SessionId source = fixture.make_source("deterministic");
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "first request", "first answer");
    append_turn(*session, 2, "second request", "second answer");

    payload::ToolCall call;
    call.id        = make_event_id().value;
    call.turn      = 2;
    call.name      = "read";
    call.arguments = nlohmann::json{{"file_path", "src/main.cpp"}};
    session->append(call);

    payload::ToolResult result;
    result.id      = call.id;
    result.name    = "read";
    result.outcome = payload::ToolOutcome::Error;
    result.error   = "no such file";
    session->append(result);

    const std::string first  = build_handoff_evidence(session->header(), session->events());
    const std::string second = build_handoff_evidence(session->header(), session->events());
    EXPECT_EQ(first, second);
    EXPECT_NE(first.find("turn_count: 2"), std::string::npos);
    EXPECT_NE(first.find("last_turn: completed"), std::string::npos);
    EXPECT_NE(first.find("src/main.cpp"), std::string::npos);
    EXPECT_NE(first.find("- read: no such file"), std::string::npos);
}

TEST(HandoffTest, HS_U3_EmptySessionIsANoOp) {
    Fixture fixture;
    const SessionId source = fixture.make_source("empty");
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);

    HandoffService    service(fixture.provider.runtime(), fixture.pool, fixture.sessions,
                              fixture.env, nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{true, {}, {}}, CancellationToken{});
    EXPECT_EQ(result.outcome, HandoffResult::Outcome::Empty);
    EXPECT_EQ(result.detail, "handoff: nothing to summarize (empty session)");
    EXPECT_FALSE(std::filesystem::exists(fixture.workspace.path() / ".ymh" / "handoffs"));
    EXPECT_EQ(fixture.store.list().size(), 1u);
}

TEST(HandoffTest, HS_U4_BoundingDropsOldestTurnsAndKeepsRecent) {
    Fixture fixture;
    fixture.policy.max_input_bytes   = 700;
    fixture.policy.keep_recent_turns = 2;

    auto recording = std::make_shared<RecordingProvider>();
    LlmRuntime              runtime;
    std::optional<AdapterHandle> handle =
        runtime.register_adapter({recording->id()}, recording);
    ASSERT_TRUE(handle.has_value());

    const SessionId source = fixture.make_source("bounded");
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    for (TurnId turn = 1; turn <= 6; ++turn) {
        session->append(payload::TurnStarted{turn, payload::TurnOrigin::User});
        payload::UserMessage user_message;
        user_message.id = make_event_id().value;
        user_message.content.push_back(
            text_block("user-marker-" + std::to_string(turn) + "-" + std::string(80, 'x')));
        session->append(user_message);
        session->append(payload::TurnEnded{turn});
    }

    HandoffService    service(runtime, fixture.pool, fixture.sessions, fixture.env, nullptr,
                              fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{false, {}, {}}, CancellationToken{});
    ASSERT_EQ(result.outcome, HandoffResult::Outcome::Ok) << result.detail;
    ASSERT_FALSE(recording->last_user_text.empty());

    const std::size_t transcript_at = recording->last_user_text.find("\n[transcript]\n");
    ASSERT_NE(transcript_at, std::string::npos);
    const std::string transcript = recording->last_user_text.substr(transcript_at);
    EXPECT_EQ(transcript.find("user-marker-1-"), std::string::npos);
    EXPECT_NE(transcript.find("user-marker-6-"), std::string::npos);
}

TEST(HandoffTest, HS_U5_ContextTooLargeLeavesNoArtifact) {
    Fixture fixture;
    fixture.policy.max_input_bytes = 32;

    const SessionId source = fixture.make_source("too-large");
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, std::string(400, 'u'), std::string(400, 'a'));

    HandoffService    service(fixture.provider.runtime(), fixture.pool, fixture.sessions,
                              fixture.env, nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{true, {}, {}}, CancellationToken{});
    EXPECT_EQ(result.outcome, HandoffResult::Outcome::ContextTooLarge);
    EXPECT_FALSE(std::filesystem::exists(fixture.workspace.path() / ".ymh" / "handoffs"));
    EXPECT_EQ(fixture.store.list().size(), 1u);
}

TEST(HandoffTest, HS_U6_DefaultPathUsesSessionIdAndCreatesDirectory) {
    Fixture fixture;
    const SessionId       session = fixture.make_source("path");
    const std::filesystem::path dir = fixture.workspace.path() / ".ymh" / "handoffs";
    const std::filesystem::path path =
        resolve_handoff_path(fixture.env, "", session, kStampTime, dir);
    EXPECT_EQ(path.filename().string(), session.value + "-" + stamp_of(kStampTime) + ".md");
    EXPECT_TRUE(std::filesystem::is_directory(dir));
}

TEST(HandoffTest, HS_U7_PathEscapeThrows) {
    Fixture fixture;
    const SessionId session = fixture.make_source("escape");
    EXPECT_THROW((void)resolve_handoff_path(fixture.env, "../../etc/passwd", session, kStampTime,
                                            fixture.workspace.path() / ".ymh" / "handoffs"),
                 ToolError);
}

TEST(HandoffTest, HS_U8_CollisionAppendsSuffix) {
    Fixture fixture;
    const SessionId       session = fixture.make_source("collision");
    const std::filesystem::path dir = fixture.workspace.path() / ".ymh" / "handoffs";
    const std::filesystem::path first =
        resolve_handoff_path(fixture.env, "", session, kStampTime, dir);
    std::ofstream{first, std::ios::binary} << "x";
    const std::filesystem::path second =
        resolve_handoff_path(fixture.env, "", session, kStampTime, dir);
    EXPECT_EQ(second.filename().string(),
              session.value + "-" + stamp_of(kStampTime) + "-2.md");
}

TEST(HandoffTest, HS_U9_WriteFailureCreatesNoSeed) {
    Fixture fixture;
    const SessionId source = fixture.make_source("write-fail");
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "hello", "world");

    const std::filesystem::path dir = fixture.workspace.path() / ".ymh" / "handoffs";
    std::filesystem::create_directories(dir.parent_path());
    std::ofstream{dir, std::ios::binary} << "not a directory";

    HandoffService    service(fixture.provider.runtime(), fixture.pool, fixture.sessions,
                              fixture.env, nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{true, {}, {}}, CancellationToken{});
    EXPECT_EQ(result.outcome, HandoffResult::Outcome::WriteFailed);
    EXPECT_EQ(fixture.store.list().size(), 1u);
}

TEST(HandoffTest, HS_I20_NoRouteLeavesNoArtifact) {
    Fixture fixture;
    const SessionId source = fixture.make_source("no-route");
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "hello", "world");

    LlmRuntime        empty_runtime;
    HandoffService    service(empty_runtime, fixture.pool, fixture.sessions, fixture.env, nullptr,
                              fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{true, {}, {}}, CancellationToken{});
    EXPECT_EQ(result.outcome, HandoffResult::Outcome::NoRoute);
    EXPECT_FALSE(std::filesystem::exists(fixture.workspace.path() / ".ymh" / "handoffs"));
    EXPECT_EQ(fixture.store.list().size(), 1u);
}

TEST(HandoffTest, HS_U10_DocIsAscii) {
    Fixture fixture;
    ProviderRuntime custom{FakeScript{{text_step(
        "## Goal\ncaf\xC3\xA9\n## Key Decisions\n- d\n## Current State\n- s\n"
        "## Next Steps\n- n\n## Open Questions\n- q\n## Relevant Files\n- f\n")}}};
    const SessionId source = fixture.make_source("ascii");
    std::shared_ptr<Session> session = fixture.sessions.sessionPtr(source);
    append_turn(*session, 1, "hello", "world");

    HandoffService    service(custom.runtime(), fixture.pool, fixture.sessions, fixture.env,
                              nullptr, fixture.policy, fixed_clock);
    const HandoffResult result = service.run(*session, session->deriveMessages(),
                                             HandoffOptions{true, {}, {}}, CancellationToken{});
    ASSERT_EQ(result.outcome, HandoffResult::Outcome::Ok);
    const std::string doc = read_file(fixture.workspace.path() / result.doc_relative);
    for (const char character : doc) {
        EXPECT_LT(static_cast<unsigned char>(character), 0x80u);
    }
}

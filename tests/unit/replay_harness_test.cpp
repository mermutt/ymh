#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/replay_harness.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/tools/builtin_tools.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

EventRecord record(Sequence seq, const SessionId& session, EventType type, nlohmann::json payload) {
    EventRecord result;
    result.seq                 = seq;
    result.event.id            = make_event_id();
    result.event.session_id    = session;
    result.event.timestamp     = std::chrono::system_clock::now();
    result.event.type          = type;
    result.event.payload       = std::move(payload);
    return result;
}

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    message.content.push_back(text_block(std::move(text)));
    return message;
}

struct RecordedAttempt {
    ToolRegistry                     registry;
    RegistrationKeeper               keeper;
    payload::LlmRequestHeader        header;
    EventRange                       log;
    EventRange                       prefix;
    Sequence                         settlement_seq = 0;
    SessionId                        session;

    explicit RecordedAttempt(ToolEnv& env)
        : keeper(registry) {
        for (std::unique_ptr<Tool>& tool : make_builtin_tools()) {
            keeper.add(std::move(tool));
        }
        registry.freeze();

        const std::string prompt = "you are a replay test";
        session                  = env.header.id;

        LLMRequest request;
        request.model      = "test-model";
        request.session_id = session;
        Message system;
        system.role = Role::System;
        system.content.push_back(text_block(prompt));
        request.messages.push_back(std::move(system));
        request.tools = registry.schemas();

        LlmCallConfig config;
        config.provider = "fake";
        config.model    = "test-model";
        FrozenRequest frozen = FrozenRequest::freeze(std::move(request), config);

        header.turn       = 1;
        header.step       = 1;
        header.session_id = session;
        header.config     = config;
        header.system_prompt        = prompt;
        header.system_prompt_digest = sha256_hex(prompt);
        header.template_digest      = frozen.template_digest();
        for (const ToolSchema& schema : registry.schemas()) {
            header.tool_names.push_back(schema.name.value);
            header.tool_schema_digests.push_back(tool_schema_digest(schema));
        }

        log.push_back(record(10, session, EventType::LlmRequestHeader, header));
        log.push_back(record(11, session, EventType::UserMessage,
                             payload::UserMessage{"u1", {text_block("hi")}}));
        payload::AssistantMessage message;
        message.id      = "a1";
        message.content = {text_block("hello")};
        log.push_back(record(12, session, EventType::AssistantMessage, message));
        settlement_seq = 12;
        prefix         = EventRange{log[0], log[1]};
    }

    ReplayEnv env() const { return ReplayEnv{&registry, std::nullopt}; }
};

TEST(ReplayHarness, VerifiesARecordedAttempt) {
    ToolEnv session_env("replay_ok");
    RecordedAttempt attempt(session_env);

    const ReplayReport report = assert_reconstructable(
        session_env.header, attempt.log, attempt.prefix, attempt.settlement_seq, attempt.header,
        attempt.env());
    EXPECT_EQ(report.status, ReplayStatus::Verified);
    EXPECT_FALSE(report.canonical_json.empty());

    const std::vector<Message> messages = reconstruct_messages(
        session_env.header, attempt.log, attempt.prefix, attempt.settlement_seq, attempt.header,
        attempt.env());
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].role, Role::System);
    EXPECT_EQ(messages[1].role, Role::User);
}

TEST(ReplayHarness, HeaderBoundedPrefixIsRejected) {
    ToolEnv session_env("replay_prefix");
    RecordedAttempt attempt(session_env);

    const EventRange header_bounded{attempt.log[0]};
    EXPECT_THROW(
        static_cast<void>(assert_reconstructable(session_env.header, attempt.log, header_bounded,
                                                 attempt.settlement_seq, attempt.header,
                                                 attempt.env())),
        ReplayMismatch);
    try {
        static_cast<void>(assert_reconstructable(session_env.header, attempt.log, header_bounded,
                                                 attempt.settlement_seq, attempt.header,
                                                 attempt.env()));
    } catch (const ReplayMismatch& mismatch) {
        EXPECT_EQ(mismatch.kind(), ReplayMismatchKind::PrefixMismatch);
    }
}

TEST(ReplayHarness, MissingHeaderIsReported) {
    ToolEnv session_env("replay_missing");
    RecordedAttempt attempt(session_env);

    EventRange no_header{attempt.log[1], attempt.log[2]};
    EventRange prefix{attempt.log[1]};
    try {
        static_cast<void>(assert_reconstructable(session_env.header, no_header, prefix,
                                                 attempt.settlement_seq, attempt.header,
                                                 attempt.env()));
        FAIL() << "expected ReplayMismatch";
    } catch (const ReplayMismatch& mismatch) {
        EXPECT_EQ(mismatch.kind(), ReplayMismatchKind::MissingHeader);
    }
}

TEST(ReplayHarness, PromptUnavailableWhenPromptTextIsUnknown) {
    ToolEnv session_env("replay_noprompt");
    RecordedAttempt attempt(session_env);
    attempt.header.system_prompt = std::nullopt;

    const ReplayReport report = assert_reconstructable(
        session_env.header, attempt.log, attempt.prefix, attempt.settlement_seq, attempt.header,
        attempt.env());
    EXPECT_EQ(report.status, ReplayStatus::PromptUnavailable);
}

TEST(ReplayHarness, ToolNameAndSchemaMismatchesAreLoud) {
    ToolEnv session_env("replay_tools");
    RecordedAttempt attempt(session_env);

    payload::LlmRequestHeader renamed = attempt.header;
    renamed.tool_names[0]             = "not-a-real-tool";
    try {
        static_cast<void>(assert_reconstructable(session_env.header, attempt.log, attempt.prefix,
                                                 attempt.settlement_seq, renamed, attempt.env()));
        FAIL() << "expected ReplayMismatch";
    } catch (const ReplayMismatch& mismatch) {
        EXPECT_EQ(mismatch.kind(), ReplayMismatchKind::ToolNameSetMismatch);
    }

    payload::LlmRequestHeader mutated = attempt.header;
    mutated.tool_schema_digests[0]    = "deadbeef";
    try {
        static_cast<void>(assert_reconstructable(session_env.header, attempt.log, attempt.prefix,
                                                 attempt.settlement_seq, mutated, attempt.env()));
        FAIL() << "expected ReplayMismatch";
    } catch (const ReplayMismatch& mismatch) {
        EXPECT_EQ(mismatch.kind(), ReplayMismatchKind::ToolSchemaMismatch);
    }
}

class OneShotCompactor final : public Compactor {
public:
    int calls = 0;

    std::optional<payload::ContextCompaction> run(const Session& session,
                                                  const std::vector<Message>&,
                                                  CancellationToken) override {
        ++calls;
        payload::ContextCompaction compaction;
        const EventRange           events = session.events();
        compaction.boundary      = events.empty() ? 0 : events.back().seq;
        compaction.summary       = "compacted";
        compaction.tokenEstimate = 1;
        compaction.model         = "fake";
        return compaction;
    }
};

// A `FakeLLM` whose every `stream()` first appends a noise record to a separate
// session in the same store. Because the store's sequence is DB-wide, this makes
// the recorded session's sequences non-contiguous (the exact `28`/`34` Rev-1
// MEDIUM shape), while the responses still come from the real FakeLLM.
class InterleavingFakeProvider final : public LLMProvider {
public:
    explicit InterleavingFakeProvider(FakeScript script) : fake_(std::move(script)) {}

    ProviderId id() const override { return fake_.id(); }
    ProviderCapabilities capabilities() const override { return fake_.capabilities(); }

    Task<LLMResponse> stream(const LLMRequest& request, StreamSink sink,
                             CancellationToken cancel) override {
        if (noise_) {
            noise_();
        }
        return fake_.stream(request, std::move(sink), cancel);
    }

    std::function<void()> noise_;

private:
    FakeLLM fake_;
};

TEST(ReplayHarness, RebuildsAreByteIdenticalAcrossCalls) {
    // 28 §4.2 / 34 §9.5 item 4: `canonical_json()` carries no timestamps or
    // ids, so independent rebuilds of the same attempt are byte-identical. The
    // harness's `MessageDivergence` guard is the defensive branch over this
    // predicate; it is unreachable while the rebuild is deterministic.
    ToolEnv       session_env("replay_determinism");
    RecordedAttempt attempt(session_env);

    const ReplayReport first = assert_reconstructable(
        session_env.header, attempt.log, attempt.prefix, attempt.settlement_seq, attempt.header,
        attempt.env());
    const ReplayReport second = assert_reconstructable(
        session_env.header, attempt.log, attempt.prefix, attempt.settlement_seq, attempt.header,
        attempt.env());
    EXPECT_EQ(first.canonical_json, second.canonical_json);
}

TEST(ReplayHarness, RealRecordedRetryWithNonContiguousSequences) {
    FakeResponseStep overflow;
    LLMError         error;
    error.code     = LLMErrorCode::ContextLengthExceeded;
    overflow.error = error;
    FakeResponseStep recovered;
    recovered.text   = "attempt-one-text";
    recovered.finish = FinishReason::Stop;

    OneShotCompactor compactor;
    AgentConfig      config;
    config.system_prompt       = "you are a replay test";
    config.persist_prompt_text = true;

    auto provider = std::make_unique<InterleavingFakeProvider>(
        FakeScript{{overflow, recovered}});
    InterleavingFakeProvider* provider_raw = provider.get();

    AgentEnv env("replay_real_retry", std::move(provider), config,
                 allow_all_permission_config(), {}, false, 4, &compactor);

    SessionHeader noise_header;
    noise_header.id = SessionId{"replay-noise"};
    env.store.create(noise_header);
    provider_raw->noise_ = [&env, &noise_header] {
        Event noise;
        noise.id         = make_event_id();
        noise.session_id = noise_header.id;
        noise.timestamp  = std::chrono::system_clock::now();
        noise.type       = EventType::SessionStarted;
        noise.payload    = nlohmann::json::object();
        env.store.append(noise_header.id, noise);
    };

    auto   agent_owner = env.createAgent();
    Agent& agent       = *agent_owner;
    ASSERT_EQ(agent.send(user_message("go")), InboxResult::Accepted);

    auto             session_owner = env.sessionOf(agent);
    Session&         session       = *session_owner;
    const EventRange log           = session.events();

    Sequence                                 settlement_seq = 0;
    std::optional<payload::LlmRequestHeader> header;
    Sequence                                 header_seq = 0;
    std::size_t                              header_count = 0;
    for (const EventRecord& record : log) {
        if (record.event.type == EventType::AssistantMessage) {
            settlement_seq = record.seq;
        } else if (record.event.type == EventType::LlmRequestHeader) {
            header     = record.event.payload.get<payload::LlmRequestHeader>();
            header_seq = record.seq;
            ++header_count;
        }
    }
    ASSERT_NE(settlement_seq, 0u);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header_count, 1u);
    EXPECT_EQ(compactor.calls, 1);

    EventRange prefix;
    for (const EventRecord& record : log) {
        if (record.seq < settlement_seq) {
            prefix.push_back(record);
        }
    }
    ASSERT_FALSE(prefix.empty());
    EXPECT_GT(prefix.back().seq, header_seq);
    EXPECT_NE(prefix.back().seq + 1, settlement_seq);

    const ReplayEnv replay_env{&env.tools, std::nullopt};
    const ReplayReport report = assert_reconstructable(
        session.header(), log, prefix, settlement_seq, *header, replay_env);
    EXPECT_EQ(report.status, ReplayStatus::Verified);
    EXPECT_FALSE(report.canonical_json.empty());

    EventRange header_bounded;
    for (const EventRecord& record : log) {
        if (record.seq <= header_seq) {
            header_bounded.push_back(record);
        }
    }
    try {
        static_cast<void>(assert_reconstructable(session.header(), log, header_bounded,
                                                 settlement_seq, *header, replay_env));
        FAIL() << "expected PrefixMismatch";
    } catch (const ReplayMismatch& mismatch) {
        EXPECT_EQ(mismatch.kind(), ReplayMismatchKind::PrefixMismatch);
    }
}

TEST(ReplayHarness, ToolMismatchIsLoudEvenWhenPromptTextIsUnknown) {
    ToolEnv session_env("replay_tools_noprompt");
    RecordedAttempt attempt(session_env);
    attempt.header.system_prompt = std::nullopt;

    payload::LlmRequestHeader renamed = attempt.header;
    renamed.tool_names[0]             = "not-a-real-tool";
    try {
        static_cast<void>(assert_reconstructable(session_env.header, attempt.log, attempt.prefix,
                                                 attempt.settlement_seq, renamed, attempt.env()));
        FAIL() << "expected ReplayMismatch";
    } catch (const ReplayMismatch& mismatch) {
        EXPECT_EQ(mismatch.kind(), ReplayMismatchKind::ToolNameSetMismatch);
    }
}

TEST(ReplayHarness, PromptAndTemplateDigestMismatchesAreLoud) {
    ToolEnv session_env("replay_digest");
    RecordedAttempt attempt(session_env);

    payload::LlmRequestHeader bad_prompt = attempt.header;
    bad_prompt.system_prompt_digest      = "deadbeef";
    try {
        static_cast<void>(assert_reconstructable(session_env.header, attempt.log, attempt.prefix,
                                                 attempt.settlement_seq, bad_prompt,
                                                 attempt.env()));
        FAIL() << "expected ReplayMismatch";
    } catch (const ReplayMismatch& mismatch) {
        EXPECT_EQ(mismatch.kind(), ReplayMismatchKind::PromptDigestMismatch);
    }

    payload::LlmRequestHeader bad_template = attempt.header;
    bad_template.template_digest           = "deadbeef";
    try {
        static_cast<void>(assert_reconstructable(session_env.header, attempt.log, attempt.prefix,
                                                 attempt.settlement_seq, bad_template,
                                                 attempt.env()));
        FAIL() << "expected ReplayMismatch";
    } catch (const ReplayMismatch& mismatch) {
        EXPECT_EQ(mismatch.kind(), ReplayMismatchKind::TemplateDigestMismatch);
    }
}

} // namespace

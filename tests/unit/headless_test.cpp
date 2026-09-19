#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "support/test_env.hpp"
#include "ymh/cli/headless.hpp"
#include "ymh/cli/stream_receiver.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/session/session_persistence.hpp"

namespace {

using namespace ymh;

FakeScript text_script(std::string text) {
    FakeScript script;
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    script.steps.push_back(std::move(step));
    return script;
}

class BlockingProvider final : public LLMProvider {
public:
    ProviderId id() const override { return "blocking"; }
    ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest& request,
                             StreamSink sink,
                             CancellationToken cancel) override {
        sink(StreamEvent{TextDelta{"partial"}});
        for (int attempt = 0; attempt < 400 && !cancel.cancelled(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        LLMResponse response;
        response.request_id = request.request_id;
        if (cancel.cancelled()) {
            response.outcome = StreamOutcome::Cancelled;
            response.finish  = FinishReason::Other;
        } else {
            response.outcome = StreamOutcome::Completed;
            response.finish  = FinishReason::Stop;
            sink(StreamEvent{Finished{FinishReason::Stop, std::nullopt, std::nullopt}});
        }
        return Task<LLMResponse>{std::move(response)};
    }
};

// Emits text and then a ContextLengthExceeded failure on the first call, and a
// completed text on the second: the retried attempt's text must not be
// concatenated with the failed attempt's (34 §15 item 1).
class OverflowAfterTextProvider final : public LLMProvider {
public:
    ProviderId id() const override { return "overflow-after-text"; }
    ProviderCapabilities capabilities() const override { return {}; }

    Task<LLMResponse> stream(const LLMRequest&, StreamSink sink, CancellationToken) override {
        ++calls;
        if (calls == 1) {
            sink(StreamEvent{TextDelta{"attempt-zero-text"}});
            LLMResponse failed;
            failed.outcome      = StreamOutcome::Failed;
            failed.finish       = FinishReason::Error;
            failed.error.code   = LLMErrorCode::ContextLengthExceeded;
            failed.error.detail = "overflow";
            sink(StreamEvent{StreamError{failed.error}});
            return Task<LLMResponse>{failed};
        }
        sink(StreamEvent{TextDelta{"attempt-one-text"}});
        sink(StreamEvent{Finished{FinishReason::Stop, std::nullopt, std::nullopt}});
        LLMResponse done;
        done.outcome = StreamOutcome::Completed;
        done.finish  = FinishReason::Stop;
        return Task<LLMResponse>{done};
    }

    int calls = 0;
};

class HeadlessTest : public ::testing::Test {
protected:
    void SetUp() override {
        LoggingOptions options;
        options.level = LogLevel::Error;
        init_logging(options);
    }
};

TEST_F(HeadlessTest, RunsFakeTurnAndPersistsSession) {
    test::TempWorkspace workspace("headless_text");
    std::ostringstream out;
    std::ostringstream err;

    HeadlessOptions options;
    options.workspace        = workspace.path();
    options.task             = "say pong";
    options.out              = &out;
    options.err              = &err;
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<FakeLLM>(text_script("pong"));
    };

    const HeadlessResult result = run_headless(options);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.terminal, "turn/end");
    EXPECT_EQ(result.assistant_text, "pong");
    EXPECT_FALSE(result.session.value.empty());
    EXPECT_NE(out.str().find("pong"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(workspace.path() / ".ymh" / "sessions.db"));
}

TEST_F(HeadlessTest, ExecutesToolCallAndPrintsActivity) {
    test::TempWorkspace workspace("headless_tool");
    workspace.write("hello.txt", "file-body");
    std::ostringstream out;
    std::ostringstream err;

    HeadlessOptions options;
    options.workspace = workspace.path();
    options.task      = "read hello.txt";
    options.out       = &out;
    options.err       = &err;
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        FakeScript script;
        FakeResponseStep call;
        call.tool_calls.push_back(FakeToolCallStep{"read_file", {{"path", "hello.txt"}}, std::nullopt});
        call.finish = FinishReason::ToolCalls;
        script.steps.push_back(std::move(call));
        FakeResponseStep answer;
        answer.text   = "done";
        answer.finish = FinishReason::Stop;
        script.steps.push_back(std::move(answer));
        return std::make_unique<FakeLLM>(std::move(script));
    };

    const HeadlessResult result = run_headless(options);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.assistant_text, "done");
    EXPECT_NE(out.str().find("read_file"), std::string::npos);
    EXPECT_NE(out.str().find("file-body"), std::string::npos);
}

TEST_F(HeadlessTest, ProviderFailureSetsNonZeroExit) {
    test::TempWorkspace workspace("headless_fail");
    std::ostringstream out;
    std::ostringstream err;

    HeadlessOptions options;
    options.workspace = workspace.path();
    options.task      = "do something";
    options.out       = &out;
    options.err       = &err;
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        FakeScript script;
        FakeResponseStep step;
        LLMError error;
        error.code   = LLMErrorCode::RateLimited;
        error.detail = "rate limited";
        step.error   = error;
        script.steps.push_back(std::move(step));
        return std::make_unique<FakeLLM>(std::move(script));
    };

    const HeadlessResult result = run_headless(options);
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_EQ(result.terminal, "turn/fail");
    EXPECT_NE(err.str().find("turn failed"), std::string::npos);
}

TEST_F(HeadlessTest, RetriedAttemptTextIsNotDoublePrinted) {
    test::TempWorkspace workspace("headless_retry_dedup");
    std::ostringstream out;
    std::ostringstream err;

    HeadlessOptions options;
    options.workspace = workspace.path();
    options.task      = "go";
    options.out       = &out;
    options.err       = &err;
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<OverflowAfterTextProvider>();
    };

    const HeadlessResult result = run_headless(options);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.terminal, "turn/end");
    EXPECT_EQ(result.assistant_text, "attempt-one-text");

    const std::string stdout_text = out.str();
    EXPECT_NE(stdout_text.find("attempt-one-text"), std::string::npos) << stdout_text;
    EXPECT_EQ(stdout_text.find("attempt-zero-text"), std::string::npos) << stdout_text;
}

TEST_F(HeadlessTest, CancellationProducesTurnCancelled) {
    test::TempWorkspace workspace("headless_cancel");
    std::ostringstream out;
    std::ostringstream err;

    HeadlessOptions options;
    options.workspace = workspace.path();
    options.task      = "long task";
    options.out       = &out;
    options.err       = &err;
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<BlockingProvider>();
    };
    options.cancel_poll = []() { return true; };

    const HeadlessResult result = run_headless(options);
    EXPECT_EQ(result.exit_code, 130);
    EXPECT_EQ(result.terminal, "turn/cancel");
}

TEST_F(HeadlessTest, ResumeAppendsToExistingSession) {
    test::TempWorkspace workspace("headless_resume");
    std::ostringstream out;
    std::ostringstream err;

    HeadlessOptions first;
    first.workspace = workspace.path();
    first.task      = "first";
    first.out       = &out;
    first.err       = &err;
    first.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<FakeLLM>(text_script("first-answer"));
    };
    const HeadlessResult first_result = run_headless(first);
    ASSERT_EQ(first_result.exit_code, 0);

    std::ostringstream second_out;
    std::ostringstream second_err;
    HeadlessOptions    second;
    second.workspace = workspace.path();
    second.task      = "second";
    second.out       = &second_out;
    second.err       = &second_err;
    second.resume    = first_result.session;
    second.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<FakeLLM>(text_script("second-answer"));
    };

    const HeadlessResult second_result = run_headless(second);
    EXPECT_EQ(second_result.exit_code, 0);
    EXPECT_EQ(second_result.session.value, first_result.session.value);
    EXPECT_EQ(second_result.assistant_text, "second-answer");
}

TEST_F(HeadlessTest, DurableMessageWithoutLiveChunksIsHandled) {
    test::TempWorkspace workspace("headless_durable_only");
    std::ostringstream out;
    std::ostringstream err;

    HeadlessOptions options;
    options.workspace = workspace.path();
    options.task      = "no visible text";
    options.out       = &out;
    options.err       = &err;
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        FakeScript script;
        FakeResponseStep step;
        step.text   = "";
        step.finish = FinishReason::Stop;
        script.steps.push_back(std::move(step));
        return std::make_unique<FakeLLM>(std::move(script));
    };

    const HeadlessResult result = run_headless(options);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.terminal, "turn/end");
    EXPECT_TRUE(result.assistant_text.empty());
}

TEST_F(HeadlessTest, BusyWorkspaceReportsActionableError) {
    test::TempWorkspace workspace("headless_busy");
    std::filesystem::create_directories(workspace.path() / ".ymh");

    PersistenceConfig held;
    held.db_path   = workspace.path() / ".ymh" / "sessions.db";
    held.lock_path = workspace.path() / ".ymh" / "sessions.lock";
    held.boot_id   = BootId{"busy-holder"};
    std::unique_ptr<SessionPersistence> holder = SessionPersistence::open(held);

    std::ostringstream out;
    std::ostringstream err;
    HeadlessOptions    options;
    options.workspace        = workspace.path();
    options.task             = "say pong";
    options.out              = &out;
    options.err              = &err;
    options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
        return std::make_unique<FakeLLM>(text_script("pong"));
    };

    const HeadlessResult result = run_headless(options);
    EXPECT_EQ(result.exit_code, 2);
    EXPECT_NE(err.str().find("workspace is busy"), std::string::npos) << err.str();
    EXPECT_NE(err.str().find("sessions.lock"), std::string::npos) << err.str();
    holder->close();
}

TEST(StreamReceiver, UnknownWireTypeIsSkippedWithoutDispatch) {
    nlohmann::json envelope_json = {
        {"session", "s"},
        {"event",
         {{"id", "e1"},
          {"session_id", "s"},
          {"timestamp", 0},
          {"type", "future/unknown_event"},
          {"payload", nlohmann::json::object()}}},
    };
    protocol::SessionEnvelope envelope;
    protocol::from_json(envelope_json, envelope);
    ASSERT_TRUE(envelope.event_skipped);

    protocol::StreamNotification stream;
    stream.envelope = envelope;

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(handle_stream_notification(stream, out, err), StreamDisposition::Skipped);
    EXPECT_TRUE(out.str().empty());
    EXPECT_TRUE(err.str().empty());
}

TEST(StreamReceiver, KnownEventTypesDispatch) {
    protocol::StreamNotification stream;
    stream.envelope.session = SessionId{"s"};
    stream.envelope.event.type = EventType::TurnEnded;

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(handle_stream_notification(stream, out, err), StreamDisposition::TurnFinished);
    EXPECT_EQ(out.str(), "\n");
    EXPECT_TRUE(err.str().empty());
}

TEST(StreamReceiver, LiveChunkSuppressesDurableDuplicateText) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    protocol::LiveNotification live;
    live.envelope.session    = SessionId{"s"};
    live.envelope.event.type = EventType::AssistantChunk;
    payload::AssistantChunk chunk;
    chunk.message = "m1";
    chunk.text    = "hello";
    chunk.kind    = payload::AssistantChunkKind::Text;
    live.envelope.event.payload = chunk;
    EXPECT_EQ(handle_live_notification(live, out, err, &printer), StreamDisposition::Continue);
    EXPECT_TRUE(out.str().empty());

    protocol::StreamNotification stream;
    stream.envelope.session    = SessionId{"s"};
    stream.envelope.event.type = EventType::AssistantMessage;
    payload::AssistantMessage message;
    message.id = "m1";
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "hello";
    message.content.push_back(block);
    stream.envelope.event.payload = message;
    EXPECT_EQ(handle_stream_notification(stream, out, err, &printer), StreamDisposition::Continue);
    EXPECT_EQ(out.str(), "hello");
}

TEST(StreamReceiver, RetriedAttemptTextIsNotDoublePrinted) {
    AssistantStreamPrinter printer;
    std::ostringstream    out;
    std::ostringstream    err;

    const auto live_text = [&](std::string text) {
        protocol::LiveNotification live;
        live.envelope.session    = SessionId{"s"};
        live.envelope.event.type = EventType::AssistantChunk;
        payload::AssistantChunk chunk;
        chunk.message = "m1";
        chunk.text    = std::move(text);
        chunk.kind    = payload::AssistantChunkKind::Text;
        live.envelope.event.payload = chunk;
        return handle_live_notification(live, out, err, &printer);
    };

    EXPECT_EQ(live_text("attempt-zero-text"), StreamDisposition::Continue);
    protocol::StreamNotification failed;
    failed.envelope.session    = SessionId{"s"};
    failed.envelope.event.type = EventType::AssistantAttempt;
    payload::AssistantAttempt attempt;
    attempt.turn = 1;
    attempt.step = 1;
    failed.envelope.event.payload = attempt;
    EXPECT_EQ(handle_stream_notification(failed, out, err, &printer), StreamDisposition::Continue);

    EXPECT_EQ(live_text("attempt-one-text"), StreamDisposition::Continue);
    protocol::StreamNotification done;
    done.envelope.session    = SessionId{"s"};
    done.envelope.event.type = EventType::AssistantMessage;
    payload::AssistantMessage message;
    message.id = "m1";
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "attempt-one-text";
    message.content.push_back(block);
    done.envelope.event.payload = message;
    EXPECT_EQ(handle_stream_notification(done, out, err, &printer), StreamDisposition::Continue);

    EXPECT_EQ(out.str().find("attempt-zero-text"), std::string::npos) << out.str();
    EXPECT_NE(out.str().find("attempt-one-text"), std::string::npos) << out.str();
}

TEST(StreamReceiver, DurableAssistantMessagePrintsAssembledText) {
    protocol::StreamNotification stream;
    stream.envelope.session    = SessionId{"s"};
    stream.envelope.event.type = EventType::AssistantMessage;

    payload::AssistantMessage message;
    message.id = "a1";
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "durable text";
    message.content.push_back(block);
    stream.envelope.event.payload = message;

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(handle_stream_notification(stream, out, err), StreamDisposition::Continue);
    EXPECT_EQ(out.str(), "durable text");
    EXPECT_TRUE(err.str().empty());
}

} // namespace

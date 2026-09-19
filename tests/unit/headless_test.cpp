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
            sink(StreamEvent{Finished{FinishReason::Stop, std::nullopt}});
        }
        return Task<LLMResponse>{std::move(response)};
    }
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

} // namespace

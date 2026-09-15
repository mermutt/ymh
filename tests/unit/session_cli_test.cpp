#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "support/test_env.hpp"
#include "ymh/cli/headless.hpp"
#include "ymh/cli/session_cli.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/llm/fake_llm.hpp"

namespace {

using namespace ymh;

FakeScript single_text(std::string text) {
    FakeScript script;
    FakeResponseStep step;
    step.text   = std::move(text);
    step.finish = FinishReason::Stop;
    script.steps.push_back(std::move(step));
    return script;
}

class SessionCliTest : public ::testing::Test {
protected:
    void SetUp() override {
        LoggingOptions options;
        options.level = LogLevel::Error;
        init_logging(options);
    }

    HeadlessResult createSession(const test::TempWorkspace& workspace) {
        std::ostringstream out;
        std::ostringstream err;
        HeadlessOptions    options;
        options.workspace = workspace.path();
        options.task      = "create a session";
        options.out       = &out;
        options.err       = &err;
        options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            return std::make_unique<FakeLLM>(single_text("hello from agent"));
        };
        return run_headless(options);
    }
};

TEST_F(SessionCliTest, ListEmptyWorkspace) {
    test::TempWorkspace workspace("session_cli_empty");
    std::ostringstream  out;
    std::ostringstream  err;
    EXPECT_EQ(session_list(workspace.path(), out, err), 0);
    EXPECT_NE(out.str().find("no sessions"), std::string::npos);
}

TEST_F(SessionCliTest, ListShowReplayFork) {
    test::TempWorkspace workspace("session_cli");
    const HeadlessResult created = createSession(workspace);
    ASSERT_EQ(created.exit_code, 0);

    std::ostringstream list_out;
    std::ostringstream list_err;
    EXPECT_EQ(session_list(workspace.path(), list_out, list_err), 0);
    EXPECT_NE(list_out.str().find(created.session.value), std::string::npos);

    std::ostringstream show_out;
    std::ostringstream show_err;
    EXPECT_EQ(session_show(workspace.path(), created.session.value, show_out, show_err), 0);
    EXPECT_NE(show_out.str().find("hello from agent"), std::string::npos);

    std::ostringstream replay_out;
    std::ostringstream replay_err;
    EXPECT_EQ(session_replay(workspace.path(), created.session.value, replay_out, replay_err), 0);
    EXPECT_NE(replay_out.str().find("turn/end"), std::string::npos);

    std::ostringstream fork_out;
    std::ostringstream fork_err;
    EXPECT_EQ(session_fork(workspace.path(), created.session.value, fork_out, fork_err), 0);
    std::string child = fork_out.str();
    const std::size_t newline = child.find('\n');
    if (newline != std::string::npos) {
        child.resize(newline);
    }
    EXPECT_FALSE(child.empty());

    std::ostringstream list_after;
    EXPECT_EQ(session_list(workspace.path(), list_after, list_err), 0);
    EXPECT_NE(list_after.str().find(child), std::string::npos);
}

TEST_F(SessionCliTest, UnknownSessionRejected) {
    test::TempWorkspace workspace("session_cli_unknown");
    (void)createSession(workspace);

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(session_show(workspace.path(), "does-not-exist", out, err), 2);
    EXPECT_NE(err.str().find("unknown session"), std::string::npos);
}

} // namespace

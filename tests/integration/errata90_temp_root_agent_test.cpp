#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "support/agent_test_env.hpp"
#include "support/test_env.hpp"
#include "ymh/core/event.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/llm/fake_llm.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

class ScopedTmpdir {
public:
    explicit ScopedTmpdir(const std::filesystem::path& value) {
        if (const char* previous = std::getenv("TMPDIR"); previous != nullptr) {
            previous_ = previous;
            had_previous_ = true;
        }
        ::setenv("TMPDIR", value.c_str(), 1);
    }

    ~ScopedTmpdir() {
        if (had_previous_) {
            ::setenv("TMPDIR", previous_.c_str(), 1);
        } else {
            ::unsetenv("TMPDIR");
        }
    }

    ScopedTmpdir(const ScopedTmpdir&) = delete;
    ScopedTmpdir& operator=(const ScopedTmpdir&) = delete;

private:
    std::string previous_;
    bool        had_previous_ = false;
};

class TempTarget {
public:
    explicit TempTarget(const std::string& name) {
        path_ = canonical_tmp_root() /
                ("ymh90_agent_" + name + "_" + std::to_string(::getpid()) + "_" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
    }

    ~TempTarget() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempTarget(const TempTarget&) = delete;
    TempTarget& operator=(const TempTarget&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

PolicyRule rule(std::string tool, PolicyVerdict effect) {
    PolicyRule value;
    value.tool   = std::move(tool);
    value.effect = effect;
    value.layer  = PolicyRule::Layer::Global;
    value.id     = "rule";
    return value;
}

PermissionConfig deny_shell() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Deny;
    config.rules.push_back(rule("shell", PolicyVerdict::Deny));
    config.rules.push_back(rule("write_file", PolicyVerdict::Deny));
    return config;
}

PermissionConfig ask_all() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    return config;
}

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

FakeScript script_of(std::vector<FakeResponseStep> steps) {
    FakeScript script;
    script.steps = std::move(steps);
    return script;
}

FakeResponseStep tool_step(std::string name, nlohmann::json arguments) {
    FakeResponseStep step;
    step.tool_calls.push_back(
        FakeToolCallStep{std::move(name), std::move(arguments), std::nullopt});
    step.finish = FinishReason::ToolCalls;
    return step;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

std::size_t tool_result_count(const Session& session) {
    std::size_t count = 0;
    for (const EventRecord& record : session.events()) {
        if (record.event.type == EventType::ToolResult) {
            ++count;
        }
    }
    return count;
}

payload::ToolOutcome last_tool_outcome(const Session& session) {
    payload::ToolOutcome outcome = payload::ToolOutcome::Ok;
    for (const EventRecord& record : session.events()) {
        if (record.event.type == EventType::ToolResult) {
            outcome = record.event.payload.get<payload::ToolResult>().outcome;
        }
    }
    return outcome;
}

} // namespace

TEST(Errata90TempRootAgent, WorkspaceIsOutsideTmp) {
    const std::filesystem::path base = test_workspace_base();
    ScopedTmpdir                tmpdir(base);
    AgentEnv env("p90_tmp_agent_base",
                 std::make_unique<FakeLLM>(script_of({text_step("ok")})));
    ASSERT_FALSE(canonical_tmp_root().empty());
    EXPECT_FALSE(path_is_under_tmp_root(canonical_tmp_root(), env.workspace.path()));
}

TEST(Errata90TempRootAgent, TouchTmpAllowedWithoutPromptUnderReadOnly) {
    ScopedTmpdir  tmpdir(test_workspace_base());
    TempTarget    target("touch");
    const std::string file = (target.path() / "x").string();
    AgentEnv      env("p90_tmp_agent_touch",
                      std::make_unique<FakeLLM>(script_of(
                          {tool_step("shell", {{"command", "touch " + file}}),
                           text_step("ok")})),
                      AgentConfig{}, deny_shell(), {}, true, 4, nullptr, true,
                      std::nullopt, std::chrono::system_clock::now, false, nullptr,
                      nullptr, nullptr, ToolConfig{}, nullptr, SandboxMode::ReadOnly);
    ASSERT_FALSE(path_is_under_tmp_root(canonical_tmp_root(), env.workspace.path()));
    bool asked = false;
    env.gate->set_attention_hook(
        [&](const PermissionRequestId& id, const PermissionRequest&) {
            asked = true;
            env.gate->decide(id, payload::PermissionDecisionKind::Deny,
                             GrantScope::Once, "unexpected");
        });
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    EXPECT_FALSE(asked);
    EXPECT_TRUE(std::filesystem::exists(file));
    auto session = env.sessionOf(*agent_owner);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(last_tool_outcome(*session), payload::ToolOutcome::Ok);
}

TEST(Errata90TempRootAgent, TouchTmpAllowedWithoutPromptUnderUnrestricted) {
    ScopedTmpdir  tmpdir(test_workspace_base());
    TempTarget    target("touch_unrestricted");
    const std::string file = (target.path() / "x").string();
    AgentEnv      env("p90_tmp_agent_touch_unrestricted",
                      std::make_unique<FakeLLM>(script_of(
                          {tool_step("shell", {{"command", "touch " + file}}),
                           text_step("ok")})),
                      AgentConfig{}, deny_shell(), {}, true, 4, nullptr, true,
                      std::nullopt, std::chrono::system_clock::now, false, nullptr,
                      nullptr, nullptr, ToolConfig{}, nullptr,
                      SandboxMode::Unrestricted);
    bool asked = false;
    env.gate->set_attention_hook(
        [&](const PermissionRequestId& id, const PermissionRequest&) {
            asked = true;
            env.gate->decide(id, payload::PermissionDecisionKind::Deny,
                             GrantScope::Once, "unexpected");
        });
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    EXPECT_FALSE(asked);
    EXPECT_TRUE(std::filesystem::exists(file));
}

TEST(Errata90TempRootAgent, RecursiveRmDeniedAndDirectoryUntouched) {
    ScopedTmpdir  tmpdir(test_workspace_base());
    TempTarget    target("rm_recursive");
    const std::string command = "rm -rf " + target.path().string();
    AgentEnv      env("p90_tmp_agent_rm",
                      std::make_unique<FakeLLM>(script_of(
                          {tool_step("shell", {{"command", command}}),
                           text_step("ok")})),
                      AgentConfig{}, ask_all(), {}, true, 4, nullptr, true,
                      std::nullopt, std::chrono::system_clock::now, false, nullptr,
                      nullptr, nullptr, ToolConfig{}, nullptr);
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    EXPECT_TRUE(std::filesystem::is_directory(target.path()));
    auto session = env.sessionOf(*agent_owner);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(last_tool_outcome(*session), payload::ToolOutcome::Denied);
}

TEST(Errata90TempRootAgent, BareTmpAndDotDotRemainDenied) {
    ScopedTmpdir tmpdir(test_workspace_base());
    const std::vector<std::string> commands = {"rm -rf /tmp", "rm -rf /tmp/.."};
    for (const std::string& command : commands) {
        AgentEnv env("p90_tmp_agent_bare",
                     std::make_unique<FakeLLM>(script_of(
                         {tool_step("shell", {{"command", command}}),
                          text_step("ok")})),
                     AgentConfig{}, ask_all(), {}, true, 4, nullptr, true,
                     std::nullopt, std::chrono::system_clock::now, false, nullptr,
                     nullptr, nullptr, ToolConfig{}, nullptr);
        auto agent_owner = env.createAgent();
        ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
        auto session = env.sessionOf(*agent_owner);
        ASSERT_NE(session, nullptr);
        EXPECT_EQ(last_tool_outcome(*session), payload::ToolOutcome::Denied)
            << command;
        EXPECT_TRUE(std::filesystem::is_directory("/tmp"));
    }
}

TEST(Errata90TempRootAgent, WriteFileTmpIsNotExempt) {
    ScopedTmpdir  tmpdir(test_workspace_base());
    TempTarget    target("write_file");
    const std::string file = (target.path() / "x").string();
    AgentEnv      env("p90_tmp_agent_write",
                      std::make_unique<FakeLLM>(script_of(
                          {tool_step("write_file", {{"path", file}, {"content", "x"}}),
                           text_step("ok")})),
                      AgentConfig{}, deny_shell(), {}, true, 4, nullptr, true,
                      std::nullopt, std::chrono::system_clock::now, false, nullptr,
                      nullptr, nullptr, ToolConfig{}, nullptr);
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    EXPECT_FALSE(std::filesystem::exists(file));
    auto session = env.sessionOf(*agent_owner);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(last_tool_outcome(*session), payload::ToolOutcome::Denied);
}

TEST(Errata90TempRootAgent, UntargetedForbidIsNotLaundered) {
    ScopedTmpdir  tmpdir(test_workspace_base());
    TempTarget    target("forbid");
    const std::string file = (target.path() / "x").string();
    for (const std::string& command :
         {"touch " + file + "; :(){ :|:& };:", "touch " + file + "; mkfs /tmp/y"}) {
        AgentEnv env("p90_tmp_agent_forbid",
                     std::make_unique<FakeLLM>(script_of(
                         {tool_step("shell", {{"command", command}}),
                          text_step("ok")})),
                     AgentConfig{}, ask_all(), {}, true, 4, nullptr, true,
                     std::nullopt, std::chrono::system_clock::now, false, nullptr,
                     nullptr, nullptr, ToolConfig{}, nullptr);
        auto agent_owner = env.createAgent();
        ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
        auto session = env.sessionOf(*agent_owner);
        ASSERT_NE(session, nullptr);
        EXPECT_EQ(last_tool_outcome(*session), payload::ToolOutcome::Denied)
            << command;
        EXPECT_FALSE(std::filesystem::exists(file)) << command;
    }
}

TEST(Errata90TempRootAgent, BraceTargetAsks) {
    ScopedTmpdir tmpdir(test_workspace_base());
    AgentEnv     env("p90_tmp_agent_brace",
                     std::make_unique<FakeLLM>(script_of(
                         {tool_step("shell",
                                    {{"command", "rm -rf /tmp/{x,/etc/passwd}"}}),
                          text_step("ok")})),
                     AgentConfig{}, ask_all(), {}, true, 4, nullptr, true,
                     std::nullopt, std::chrono::system_clock::now, false, nullptr,
                     nullptr, nullptr, ToolConfig{}, nullptr);
    bool asked = false;
    env.gate->set_attention_hook(
        [&](const PermissionRequestId& id, const PermissionRequest&) {
            asked = true;
            env.gate->decide(id, payload::PermissionDecisionKind::Deny,
                             GrantScope::Once, "denied");
        });
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    EXPECT_TRUE(asked);
    EXPECT_EQ(tool_result_count(*env.sessionOf(*agent_owner)), 1u);
}

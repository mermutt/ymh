#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "support/agent_test_env.hpp"
#include "ymh/core/event.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/policy/path_target.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

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
    step.tool_calls.push_back(FakeToolCallStep{std::move(name), std::move(arguments),
                                               std::nullopt});
    step.finish = FinishReason::ToolCalls;
    return step;
}

FakeResponseStep text_step(std::string text) {
    FakeResponseStep step;
    step.text = std::move(text);
    step.finish = FinishReason::Stop;
    return step;
}

PolicyRule path_rule(std::string tool, std::string path, PolicyVerdict effect) {
    PolicyRule rule;
    rule.tool = std::move(tool);
    rule.path = std::move(path);
    rule.effect = effect;
    rule.layer = PolicyRule::Layer::Global;
    rule.id = "rule";
    return rule;
}

PermissionConfig recommended() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Ask;
    config.rules.push_back(path_rule("*", "**", PolicyVerdict::Allow));
    config.rules.push_back(path_rule("*", ".ymh/**", PolicyVerdict::Deny));
    config.rules.push_back(path_rule("*", ".git/**", PolicyVerdict::Deny));
    return config;
}


} // namespace

TEST(Errata90AgentLoopPath, CapturesWriteFileTarget) {
    AgentEnv env("p90_loop_write",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("write_file", {{"path", "src/x"}, {"content", "x"}}),
                      text_step("ok")})),
                 AgentConfig{}, PermissionConfig{}, {}, true, 4, nullptr, true);
    std::optional<std::filesystem::path> captured;
    env.gate->set_attention_hook(
        [&](const PermissionRequestId& id, const PermissionRequest& request) {
            captured = request.path;
            env.gate->decide(id, payload::PermissionDecisionKind::Allow,
                             GrantScope::Once, "ok");
        });
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(*captured, env.workspace.path() / "src" / "x");
}

TEST(Errata90AgentLoopPath, EscapingPathIsAbsoluteOutsideAndAsks) {
    AgentEnv env("p90_loop_escape",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("write_file", {{"path", "../escape"}, {"content", "x"}}),
                      text_step("ok")})),
                 AgentConfig{}, PermissionConfig{}, {}, true, 4, nullptr, true);
    std::optional<PermissionRequest> captured;
    env.gate->set_attention_hook(
        [&](const PermissionRequestId& id, const PermissionRequest& request) {
            captured = request;
            env.gate->decide(id, payload::PermissionDecisionKind::Allow,
                             GrantScope::Once, "ok");
        });
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    ASSERT_TRUE(captured.has_value());
    ASSERT_TRUE(captured->path.has_value());
    EXPECT_TRUE(captured->path->is_absolute());
    EXPECT_FALSE(path_is_within_root(captured->root, *captured->path));
}

TEST(Errata90AgentLoopPath, InRootShellAllowsWithoutPrompt) {
    AgentEnv env("p90_loop_shell",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("shell", {{"command", "git status"}}), text_step("ok")})),
                 AgentConfig{}, recommended(), {}, true, 4, nullptr, true);
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
}

TEST(Errata90AgentLoopPath, OutsideShellAsks) {
    AgentEnv env("p90_loop_shell_out",
                 std::make_unique<FakeLLM>(
                     script_of({tool_step("shell", {{"command", "cat /etc/passwd"}}),
                                text_step("ok")})),
                 AgentConfig{}, recommended(), {}, true, 4, nullptr, true);
    bool asked = false;
    env.gate->set_attention_hook(
        [&](const PermissionRequestId& id, const PermissionRequest& request) {
            asked = request.tool == "shell";
            env.gate->decide(id, payload::PermissionDecisionKind::Deny,
                             GrantScope::Once, "denied");
        });
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    EXPECT_TRUE(asked);
}

TEST(Errata90AgentLoopPath, ProtectedWriteAndGitHookDeniedBeforeTool) {
    AgentEnv env("p90_loop_protected",
                 std::make_unique<FakeLLM>(script_of(
                     {tool_step("write_file",
                                {{"path", ".ymh/config.jsonc"}, {"content", "{}"}}),
                      tool_step("shell", {{"command", "chmod +x .git/hooks/pre-commit"}}),
                      text_step("ok")})),
                 AgentConfig{}, recommended(), {}, true, 4, nullptr, true);
    bool asked = false;
    env.gate->set_attention_hook(
        [&](const PermissionRequestId& id, const PermissionRequest&) {
            asked = true;
            env.gate->decide(id, payload::PermissionDecisionKind::Allow,
                             GrantScope::Once, "ok");
        });
    auto agent_owner = env.createAgent();
    ASSERT_EQ(agent_owner->send(user_message("go")), InboxResult::Accepted);
    EXPECT_FALSE(asked);
    EXPECT_FALSE(std::filesystem::exists(env.workspace.path() / ".ymh" / "config.jsonc"));
    EXPECT_FALSE(std::filesystem::exists(env.workspace.path() / ".git"));
}
